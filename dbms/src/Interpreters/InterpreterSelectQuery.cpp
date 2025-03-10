#include <DataStreams/ExpressionBlockInputStream.h>
#include <DataStreams/FilterBlockInputStream.h>
#include <DataStreams/FinishSortingBlockInputStream.h>
#include <DataStreams/LimitBlockInputStream.h>
#include <DataStreams/LimitByBlockInputStream.h>
#include <DataStreams/PartialSortingBlockInputStream.h>
#include <DataStreams/MergeSortingBlockInputStream.h>
#include <DataStreams/MergingSortedBlockInputStream.h>
#include <DataStreams/AggregatingBlockInputStream.h>
#include <DataStreams/MergingAggregatedBlockInputStream.h>
#include <DataStreams/MergingAggregatedMemoryEfficientBlockInputStream.h>
#include <DataStreams/AsynchronousBlockInputStream.h>
#include <DataStreams/UnionBlockInputStream.h>
#include <DataStreams/ParallelAggregatingBlockInputStream.h>
#include <DataStreams/DistinctBlockInputStream.h>
#include <DataStreams/NullBlockInputStream.h>
#include <DataStreams/TotalsHavingBlockInputStream.h>
#include <DataStreams/copyData.h>
#include <DataStreams/CreatingSetsBlockInputStream.h>
#include <DataStreams/MaterializingBlockInputStream.h>
#include <DataStreams/ConcatBlockInputStream.h>
#include <DataStreams/RollupBlockInputStream.h>
#include <DataStreams/CubeBlockInputStream.h>
#include <DataStreams/ConvertColumnLowCardinalityToFullBlockInputStream.h>
#include <DataStreams/ConvertingBlockInputStream.h>
#include <DataStreams/ReverseBlockInputStream.h>

#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTOrderByElement.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ParserSelectQuery.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/parseQuery.h>

#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/InterpreterSelectWithUnionQuery.h>
#include <Interpreters/InterpreterSetQuery.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/convertFieldToType.h>
#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/DatabaseAndTableWithAlias.h>
#include <Interpreters/JoinToSubqueryTransformVisitor.h>
#include <Interpreters/CrossToInnerJoinVisitor.h>

#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreeWhereOptimizer.h>
#include <Storages/IStorage.h>
#include <Storages/StorageValues.h>

#include <TableFunctions/ITableFunction.h>
#include <TableFunctions/TableFunctionFactory.h>

#include <Functions/IFunction.h>
#include <Core/Field.h>
#include <Core/Types.h>
#include <Columns/Collator.h>
#include <Common/typeid_cast.h>
#include <Common/checkStackSize.h>
#include <Parsers/queryToString.h>
#include <ext/map.h>
#include <memory>

#include <Processors/Sources/NullSource.h>
#include <Processors/Sources/SourceFromInputStream.h>
#include <Processors/Transforms/FilterTransform.h>
#include <Processors/Transforms/ExpressionTransform.h>
#include <Processors/Transforms/AggregatingTransform.h>
#include <Processors/Transforms/MergingAggregatedTransform.h>
#include <Processors/Transforms/MergingAggregatedMemoryEfficientTransform.h>
#include <Processors/Transforms/TotalsHavingTransform.h>
#include <Processors/Transforms/PartialSortingTransform.h>
#include <Processors/Transforms/LimitsCheckingTransform.h>
#include <Processors/Transforms/MergeSortingTransform.h>
#include <Processors/Transforms/MergingSortedTransform.h>
#include <Processors/Transforms/DistinctTransform.h>
#include <Processors/Transforms/LimitByTransform.h>
#include <Processors/Transforms/ExtremesTransform.h>
#include <Processors/Transforms/CreatingSetsTransform.h>
#include <Processors/Transforms/RollupTransform.h>
#include <Processors/Transforms/CubeTransform.h>
#include <Processors/LimitTransform.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataStreams/materializeBlock.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int TOO_DEEP_SUBQUERIES;
    extern const int THERE_IS_NO_COLUMN;
    extern const int SAMPLING_NOT_SUPPORTED;
    extern const int ILLEGAL_FINAL;
    extern const int ILLEGAL_PREWHERE;
    extern const int TOO_MANY_COLUMNS;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int PARAMETER_OUT_OF_BOUND;
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int INVALID_LIMIT_EXPRESSION;
}

namespace
{

/// Assumes `storage` is set and the table filter is not empty.
String generateFilterActions(ExpressionActionsPtr & actions, const StoragePtr & storage, const Context & context, const Names & prerequisite_columns = {})
{
    const auto & db_name = storage->getDatabaseName();
    const auto & table_name = storage->getTableName();
    const auto & filter_str = context.getUserProperty(db_name, table_name, "filter");

    /// TODO: implement some AST builders for this kind of stuff
    ASTPtr query_ast = std::make_shared<ASTSelectQuery>();
    auto * select_ast = query_ast->as<ASTSelectQuery>();

    select_ast->setExpression(ASTSelectQuery::Expression::SELECT, std::make_shared<ASTExpressionList>());
    auto expr_list = select_ast->select();

    auto parseExpression = [] (const String & expr)
    {
        ParserExpression expr_parser;
        return parseQuery(expr_parser, expr, 0);
    };

    // The first column is our filter expression.
    expr_list->children.push_back(parseExpression(filter_str));

    /// Keep columns that are required after the filter actions.
    for (const auto & column_str : prerequisite_columns)
        expr_list->children.push_back(parseExpression(column_str));

    select_ast->setExpression(ASTSelectQuery::Expression::TABLES, std::make_shared<ASTTablesInSelectQuery>());
    auto tables = select_ast->tables();
    auto tables_elem = std::make_shared<ASTTablesInSelectQueryElement>();
    auto table_expr = std::make_shared<ASTTableExpression>();
    tables->children.push_back(tables_elem);
    tables_elem->table_expression = table_expr;
    tables_elem->children.push_back(table_expr);
    table_expr->database_and_table_name = createTableIdentifier(db_name, table_name);
    table_expr->children.push_back(table_expr->database_and_table_name);

    /// Using separate expression analyzer to prevent any possible alias injection
    auto syntax_result = SyntaxAnalyzer(context).analyze(query_ast, storage->getColumns().getAllPhysical());
    SelectQueryExpressionAnalyzer analyzer(query_ast, syntax_result, context);
    ExpressionActionsChain new_chain(context);
    analyzer.appendSelect(new_chain, false);
    actions = new_chain.getLastActions();

    return expr_list->children.at(0)->getColumnName();
}

}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const Context & context_,
    const SelectQueryOptions & options_,
    const Names & required_result_column_names_)
    : InterpreterSelectQuery(query_ptr_, context_, nullptr, nullptr, options_, required_result_column_names_)
{
}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const Context & context_,
    const BlockInputStreamPtr & input_,
    const SelectQueryOptions & options_)
    : InterpreterSelectQuery(query_ptr_, context_, input_, nullptr, options_.copy().noSubquery())
{}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const Context & context_,
    const StoragePtr & storage_,
    const SelectQueryOptions & options_)
    : InterpreterSelectQuery(query_ptr_, context_, nullptr, storage_, options_.copy().noSubquery())
{}

InterpreterSelectQuery::~InterpreterSelectQuery() = default;


/** There are no limits on the maximum size of the result for the subquery.
  *  Since the result of the query is not the result of the entire query.
  */
static Context getSubqueryContext(const Context & context)
{
    Context subquery_context = context;
    Settings subquery_settings = context.getSettings();
    subquery_settings.max_result_rows = 0;
    subquery_settings.max_result_bytes = 0;
    /// The calculation of extremes does not make sense and is not necessary (if you do it, then the extremes of the subquery can be taken for whole query).
    subquery_settings.extremes = 0;
    subquery_context.setSettings(subquery_settings);
    return subquery_context;
}

InterpreterSelectQuery::InterpreterSelectQuery(
    const ASTPtr & query_ptr_,
    const Context & context_,
    const BlockInputStreamPtr & input_,
    const StoragePtr & storage_,
    const SelectQueryOptions & options_,
    const Names & required_result_column_names)
    : options(options_)
    /// NOTE: the query almost always should be cloned because it will be modified during analysis.
    , query_ptr(options.modify_inplace ? query_ptr_ : query_ptr_->clone())
    , context(context_)
    , storage(storage_)
    , input(input_)
    , log(&Logger::get("InterpreterSelectQuery"))
{
    checkStackSize();

    initSettings();
    const Settings & settings = context.getSettingsRef();

    if (settings.max_subquery_depth && options.subquery_depth > settings.max_subquery_depth)
        throw Exception("Too deep subqueries. Maximum: " + settings.max_subquery_depth.toString(),
            ErrorCodes::TOO_DEEP_SUBQUERIES);

    if (settings.allow_experimental_cross_to_join_conversion)
    {
        CrossToInnerJoinVisitor::Data cross_to_inner;
        CrossToInnerJoinVisitor(cross_to_inner).visit(query_ptr);
    }

    if (settings.allow_experimental_multiple_joins_emulation)
    {
        JoinToSubqueryTransformVisitor::Data join_to_subs_data{context};
        JoinToSubqueryTransformVisitor(join_to_subs_data).visit(query_ptr);
    }

    max_streams = settings.max_threads;
    auto & query = getSelectQuery();

    ASTPtr table_expression = extractTableExpression(query, 0);

    bool is_table_func = false;
    bool is_subquery = false;
    if (table_expression)
    {
        is_table_func = table_expression->as<ASTFunction>();
        is_subquery = table_expression->as<ASTSelectWithUnionQuery>();
    }

    if (input)
    {
        /// Read from prepared input.
        source_header = input->getHeader();
    }
    else if (is_subquery)
    {
        /// Read from subquery.
        interpreter_subquery = std::make_unique<InterpreterSelectWithUnionQuery>(
            table_expression, getSubqueryContext(context), options.subquery(), required_columns);

        source_header = interpreter_subquery->getSampleBlock();
    }
    else if (!storage)
    {
        if (is_table_func)
        {
            /// Read from table function.
            storage = context.getQueryContext().executeTableFunction(table_expression);
        }
        else
        {
            String database_name;
            String table_name;

            getDatabaseAndTableNames(query, database_name, table_name, context);

            if (auto view_source = context.getViewSource())
            {
                auto & storage_values = static_cast<const StorageValues &>(*view_source);
                if (storage_values.getDatabaseName() == database_name && storage_values.getTableName() == table_name)
                {
                    /// Read from view source.
                    storage = context.getViewSource();
                }
            }

            if (!storage)
            {
                /// Read from table. Even without table expression (implicit SELECT ... FROM system.one).
                storage = context.getTable(database_name, table_name);
            }
        }
    }

    if (storage)
        table_lock = storage->lockStructureForShare(false, context.getCurrentQueryId());

    syntax_analyzer_result = SyntaxAnalyzer(context, options).analyze(
        query_ptr, source_header.getNamesAndTypesList(), required_result_column_names, storage, NamesAndTypesList());
    query_analyzer = std::make_unique<SelectQueryExpressionAnalyzer>(
        query_ptr, syntax_analyzer_result, context,
        NameSet(required_result_column_names.begin(), required_result_column_names.end()),
        options.subquery_depth, !options.only_analyze);

    if (!options.only_analyze)
    {
        if (query.sample_size() && (input || !storage || !storage->supportsSampling()))
            throw Exception("Illegal SAMPLE: table doesn't support sampling", ErrorCodes::SAMPLING_NOT_SUPPORTED);

        if (query.final() && (input || !storage || !storage->supportsFinal()))
            throw Exception((!input && storage) ? "Storage " + storage->getName() + " doesn't support FINAL" : "Illegal FINAL", ErrorCodes::ILLEGAL_FINAL);

        if (query.prewhere() && (input || !storage || !storage->supportsPrewhere()))
            throw Exception((!input && storage) ? "Storage " + storage->getName() + " doesn't support PREWHERE" : "Illegal PREWHERE", ErrorCodes::ILLEGAL_PREWHERE);

        /// Save the new temporary tables in the query context
        for (const auto & it : query_analyzer->getExternalTables())
            if (!context.tryGetExternalTable(it.first))
                context.addExternalTable(it.first, it.second);
    }

    if (!options.only_analyze || options.modify_inplace)
    {
        if (syntax_analyzer_result->rewrite_subqueries)
        {
            /// remake interpreter_subquery when PredicateOptimizer rewrites subqueries and main table is subquery
            if (is_subquery)
                interpreter_subquery = std::make_unique<InterpreterSelectWithUnionQuery>(
                    table_expression,
                    getSubqueryContext(context),
                    options.subquery(),
                    required_columns);
        }
    }

    if (interpreter_subquery)
    {
        /// If there is an aggregation in the outer query, WITH TOTALS is ignored in the subquery.
        if (query_analyzer->hasAggregation())
            interpreter_subquery->ignoreWithTotals();
    }

    required_columns = syntax_analyzer_result->requiredSourceColumns();

    if (storage)
        source_header = storage->getSampleBlockForColumns(required_columns);

    /// Calculate structure of the result.
    result_header = getSampleBlockImpl();
    for (auto & col : result_header)
    {
        if (!col.column)
            col.column = col.type->createColumn();
        else if (isColumnConst(*col.column) && !col.column->empty())
            col.column = col.column->cloneEmpty();
    }
}


void InterpreterSelectQuery::getDatabaseAndTableNames(const ASTSelectQuery & query, String & database_name, String & table_name, const Context & context)
{
    if (auto db_and_table = getDatabaseAndTable(query, 0))
    {
        table_name = db_and_table->table;
        database_name = db_and_table->database;

        /// If the database is not specified - use the current database.
        if (database_name.empty() && !context.tryGetTable("", table_name))
            database_name = context.getCurrentDatabase();
    }
    else /// If the table is not specified - use the table `system.one`.
    {
        database_name = "system";
        table_name = "one";
    }
}


Block InterpreterSelectQuery::getSampleBlock()
{
    return result_header;
}


BlockIO InterpreterSelectQuery::execute()
{
    Pipeline pipeline;
    executeImpl(pipeline, input);
    executeUnion(pipeline, getSampleBlock());

    BlockIO res;
    res.in = pipeline.firstStream();
    return res;
}

BlockInputStreams InterpreterSelectQuery::executeWithMultipleStreams()
{
    Pipeline pipeline;
    executeImpl(pipeline, input);
    unifyStreams(pipeline, getSampleBlock());
    return pipeline.streams;
}

QueryPipeline InterpreterSelectQuery::executeWithProcessors()
{
    QueryPipeline query_pipeline;
    executeImpl(query_pipeline, input);
    return query_pipeline;
}


Block InterpreterSelectQuery::getSampleBlockImpl()
{
    FilterInfoPtr filter_info;

    /// Need to create sets before analyzeExpressions(). Otherwise some sets for index won't be created.
    query_analyzer->makeSetsForIndex(getSelectQuery().where());
    query_analyzer->makeSetsForIndex(getSelectQuery().prewhere());

    auto analysis_result = analyzeExpressions(
            getSelectQuery(),
            *query_analyzer,
            QueryProcessingStage::Enum::FetchColumns,
            options.to_stage,
            context,
            storage,
            true,
            filter_info);

    if (options.to_stage == QueryProcessingStage::Enum::FetchColumns)
    {
        auto header = source_header;

        if (analysis_result.prewhere_info)
        {
            analysis_result.prewhere_info->prewhere_actions->execute(header);
            header = materializeBlock(header);
            if (analysis_result.prewhere_info->remove_prewhere_column)
                header.erase(analysis_result.prewhere_info->prewhere_column_name);
        }
        return header;
    }

    if (options.to_stage == QueryProcessingStage::Enum::WithMergeableState)
    {
        if (!analysis_result.need_aggregate)
            return analysis_result.before_order_and_select->getSampleBlock();

        auto header = analysis_result.before_aggregation->getSampleBlock();

        Names key_names;
        AggregateDescriptions aggregates;
        query_analyzer->getAggregateInfo(key_names, aggregates);

        Block res;

        for (auto & key : key_names)
            res.insert({nullptr, header.getByName(key).type, key});

        for (auto & aggregate : aggregates)
        {
            size_t arguments_size = aggregate.argument_names.size();
            DataTypes argument_types(arguments_size);
            for (size_t j = 0; j < arguments_size; ++j)
                argument_types[j] = header.getByName(aggregate.argument_names[j]).type;

            DataTypePtr type = std::make_shared<DataTypeAggregateFunction>(aggregate.function, argument_types, aggregate.parameters);

            res.insert({nullptr, type, aggregate.column_name});
        }

        return res;
    }

    return analysis_result.final_projection->getSampleBlock();
}

InterpreterSelectQuery::AnalysisResult
InterpreterSelectQuery::analyzeExpressions(
    const ASTSelectQuery & query,
    SelectQueryExpressionAnalyzer & query_analyzer,
    QueryProcessingStage::Enum from_stage,
    QueryProcessingStage::Enum to_stage,
    const Context & context,
    const StoragePtr & storage,
    bool only_types,
    const FilterInfoPtr & filter_info)
{
    AnalysisResult res;

    /// Do I need to perform the first part of the pipeline - running on remote servers during distributed processing.
    res.first_stage = from_stage < QueryProcessingStage::WithMergeableState
        && to_stage >= QueryProcessingStage::WithMergeableState;
    /// Do I need to execute the second part of the pipeline - running on the initiating server during distributed processing.
    res.second_stage = from_stage <= QueryProcessingStage::WithMergeableState
        && to_stage > QueryProcessingStage::WithMergeableState;

    /** First we compose a chain of actions and remember the necessary steps from it.
        *  Regardless of from_stage and to_stage, we will compose a complete sequence of actions to perform optimization and
        *  throw out unnecessary columns based on the entire query. In unnecessary parts of the query, we will not execute subqueries.
        */

    bool has_filter = false;
    bool has_prewhere = false;
    bool has_where = false;
    size_t where_step_num;

    auto finalizeChain = [&](ExpressionActionsChain & chain)
    {
        chain.finalize();

        if (has_prewhere)
        {
            const ExpressionActionsChain::Step & step = chain.steps.at(0);
            res.prewhere_info->remove_prewhere_column = step.can_remove_required_output.at(0);

            Names columns_to_remove;
            for (size_t i = 1; i < step.required_output.size(); ++i)
            {
                if (step.can_remove_required_output[i])
                    columns_to_remove.push_back(step.required_output[i]);
            }

            if (!columns_to_remove.empty())
            {
                auto columns = res.prewhere_info->prewhere_actions->getSampleBlock().getNamesAndTypesList();
                ExpressionActionsPtr actions = std::make_shared<ExpressionActions>(columns, context);
                for (const auto & column : columns_to_remove)
                    actions->add(ExpressionAction::removeColumn(column));

                res.prewhere_info->remove_columns_actions = std::move(actions);
            }

            res.columns_to_remove_after_prewhere = std::move(columns_to_remove);
        }
        else if (has_filter)
        {
            /// Can't have prewhere and filter set simultaneously
            res.filter_info->do_remove_column = chain.steps.at(0).can_remove_required_output.at(0);
        }
        if (has_where)
            res.remove_where_filter = chain.steps.at(where_step_num).can_remove_required_output.at(0);

        has_filter = has_prewhere = has_where = false;

        chain.clear();
    };

    {
        ExpressionActionsChain chain(context);
        Names additional_required_columns_after_prewhere;

        if (storage && query.sample_size())
        {
            Names columns_for_sampling = storage->getColumnsRequiredForSampling();
            additional_required_columns_after_prewhere.insert(additional_required_columns_after_prewhere.end(),
                columns_for_sampling.begin(), columns_for_sampling.end());
        }

        if (storage && query.final())
        {
            Names columns_for_final = storage->getColumnsRequiredForFinal();
            additional_required_columns_after_prewhere.insert(additional_required_columns_after_prewhere.end(),
                columns_for_final.begin(), columns_for_final.end());
        }

        if (storage && filter_info)
        {
            has_filter = true;

            /// XXX: aggregated copy-paste from ExpressionAnalyzer::appendSmth()
            if (chain.steps.empty())
            {
                chain.steps.emplace_back(std::make_shared<ExpressionActions>(NamesAndTypesList(), context));
            }
            ExpressionActionsChain::Step & step = chain.steps.back();

            // FIXME: assert(filter_info);
            res.filter_info = filter_info;
            step.actions = filter_info->actions;
            step.required_output.push_back(res.filter_info->column_name);
            step.can_remove_required_output = {true};

            chain.addStep();
        }

        if (query_analyzer.appendPrewhere(chain, !res.first_stage, additional_required_columns_after_prewhere))
        {
            has_prewhere = true;

            res.prewhere_info = std::make_shared<PrewhereInfo>(
                    chain.steps.front().actions, query.prewhere()->getColumnName());

            chain.addStep();
        }

        res.need_aggregate = query_analyzer.hasAggregation();

        query_analyzer.appendArrayJoin(chain, only_types || !res.first_stage);

        if (query_analyzer.appendJoin(chain, only_types || !res.first_stage))
        {
            res.before_join = chain.getLastActions();
            if (!res.hasJoin())
                throw Exception("No expected JOIN", ErrorCodes::LOGICAL_ERROR);
            chain.addStep();
        }

        if (query_analyzer.appendWhere(chain, only_types || !res.first_stage))
        {
            where_step_num = chain.steps.size() - 1;
            has_where = res.has_where = true;
            res.before_where = chain.getLastActions();
            chain.addStep();
        }

        if (res.need_aggregate)
        {
            query_analyzer.appendGroupBy(chain, only_types || !res.first_stage);
            query_analyzer.appendAggregateFunctionsArguments(chain, only_types || !res.first_stage);
            res.before_aggregation = chain.getLastActions();

            finalizeChain(chain);

            if (query_analyzer.appendHaving(chain, only_types || !res.second_stage))
            {
                res.has_having = true;
                res.before_having = chain.getLastActions();
                chain.addStep();
            }
        }

        /// If there is aggregation, we execute expressions in SELECT and ORDER BY on the initiating server, otherwise on the source servers.
        query_analyzer.appendSelect(chain, only_types || (res.need_aggregate ? !res.second_stage : !res.first_stage));
        res.selected_columns = chain.getLastStep().required_output;
        res.has_order_by = query_analyzer.appendOrderBy(chain, only_types || (res.need_aggregate ? !res.second_stage : !res.first_stage));
        res.before_order_and_select = chain.getLastActions();
        chain.addStep();

        if (query_analyzer.appendLimitBy(chain, only_types || !res.second_stage))
        {
            res.has_limit_by = true;
            res.before_limit_by = chain.getLastActions();
            chain.addStep();
        }

        query_analyzer.appendProjectResult(chain);
        res.final_projection = chain.getLastActions();

        finalizeChain(chain);
    }

    /// Before executing WHERE and HAVING, remove the extra columns from the block (mostly the aggregation keys).
    if (res.filter_info)
        res.filter_info->actions->prependProjectInput();
    if (res.has_where)
        res.before_where->prependProjectInput();
    if (res.has_having)
        res.before_having->prependProjectInput();

    res.subqueries_for_sets = query_analyzer.getSubqueriesForSets();

    /// Check that PREWHERE doesn't contain unusual actions. Unusual actions are that can change number of rows.
    if (res.prewhere_info)
    {
        auto check_actions = [](const ExpressionActionsPtr & actions)
        {
            if (actions)
                for (const auto & action : actions->getActions())
                    if (action.type == ExpressionAction::Type::JOIN || action.type == ExpressionAction::Type::ARRAY_JOIN)
                        throw Exception("PREWHERE cannot contain ARRAY JOIN or JOIN action", ErrorCodes::ILLEGAL_PREWHERE);
        };

        check_actions(res.prewhere_info->prewhere_actions);
        check_actions(res.prewhere_info->alias_actions);
        check_actions(res.prewhere_info->remove_columns_actions);
    }

    return res;
}


static SortDescription getSortDescription(const ASTSelectQuery & query)
{
    SortDescription order_descr;
    order_descr.reserve(query.orderBy()->children.size());
    for (const auto & elem : query.orderBy()->children)
    {
        String name = elem->children.front()->getColumnName();
        const auto & order_by_elem = elem->as<ASTOrderByElement &>();

        std::shared_ptr<Collator> collator;
        if (order_by_elem.collation)
            collator = std::make_shared<Collator>(order_by_elem.collation->as<ASTLiteral &>().value.get<String>());

        order_descr.emplace_back(name, order_by_elem.direction, order_by_elem.nulls_direction, collator);
    }

    return order_descr;
}


static UInt64 getLimitUIntValue(const ASTPtr & node, const Context & context)
{
    const auto & [field, type] = evaluateConstantExpression(node, context);

    if (!isNativeNumber(type))
        throw Exception("Illegal type " + type->getName() + " of LIMIT expression, must be numeric type", ErrorCodes::INVALID_LIMIT_EXPRESSION);

    Field converted = convertFieldToType(field, DataTypeUInt64());
    if (converted.isNull())
        throw Exception("The value " + applyVisitor(FieldVisitorToString(), field) + " of LIMIT expression is not representable as UInt64", ErrorCodes::INVALID_LIMIT_EXPRESSION);

    return converted.safeGet<UInt64>();
}


static std::pair<UInt64, UInt64> getLimitLengthAndOffset(const ASTSelectQuery & query, const Context & context)
{
    UInt64 length = 0;
    UInt64 offset = 0;

    if (query.limitLength())
    {
        length = getLimitUIntValue(query.limitLength(), context);
        if (query.limitOffset())
            offset = getLimitUIntValue(query.limitOffset(), context);
    }

    return {length, offset};
}


static UInt64 getLimitForSorting(const ASTSelectQuery & query, const Context & context)
{
    /// Partial sort can be done if there is LIMIT but no DISTINCT or LIMIT BY.
    if (!query.distinct && !query.limitBy())
    {
        auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context);
        return limit_length + limit_offset;
    }
    return 0;
}


static SortingInfoPtr optimizeReadInOrder(const MergeTreeData & merge_tree, const ASTSelectQuery & query,
    const Context & context, const SyntaxAnalyzerResultPtr & global_syntax_result)
{
    if (!merge_tree.hasSortingKey())
        return {};

    auto order_descr = getSortDescription(query);
    SortDescription prefix_order_descr;
    int read_direction = order_descr.at(0).direction;

    const auto & sorting_key_columns = merge_tree.getSortingKeyColumns();
    size_t prefix_size = std::min(order_descr.size(), sorting_key_columns.size());

    for (size_t i = 0; i < prefix_size; ++i)
    {
        if (global_syntax_result->array_join_result_to_source.count(order_descr[i].column_name))
            break;

        /// Optimize in case of exact match with order key element
        ///  or in some simple cases when order key element is wrapped into monotonic function.
        int current_direction = order_descr[i].direction;
        if (order_descr[i].column_name == sorting_key_columns[i] && current_direction == read_direction)
            prefix_order_descr.push_back(order_descr[i]);
        else
        {
            auto ast = query.orderBy()->children[i]->children.at(0);
            auto syntax_result = SyntaxAnalyzer(context).analyze(ast, global_syntax_result->required_source_columns);
            auto actions = ExpressionAnalyzer(ast, syntax_result, context).getActions(true);

            const auto & input_columns = actions->getRequiredColumnsWithTypes();
            if (input_columns.size() != 1 || input_columns.front().name != sorting_key_columns[i])
                break;

            bool first = true;
            for (const auto & action : actions->getActions())
            {
                if (action.type != ExpressionAction::APPLY_FUNCTION)
                    continue;

                if (!first)
                {
                    current_direction = 0;
                    break;
                }
                else
                    first = false;

                const auto & func = *action.function_base;
                if (!func.hasInformationAboutMonotonicity())
                {
                    current_direction = 0;
                    break;
                }

                auto monotonicity = func.getMonotonicityForRange(*input_columns.front().type, {}, {});
                if (!monotonicity.is_monotonic)
                {
                    current_direction = 0;
                    break;
                }
                else if (!monotonicity.is_positive)
                    current_direction *= -1;
            }

            if (!current_direction || (i > 0 && current_direction != read_direction))
                break;

            if (i == 0)
                read_direction = current_direction;

            prefix_order_descr.push_back(order_descr[i]);
        }
    }

    if (prefix_order_descr.empty())
        return {};

    return std::make_shared<SortingInfo>(std::move(prefix_order_descr), read_direction);
}


template <typename TPipeline>
void InterpreterSelectQuery::executeImpl(TPipeline & pipeline, const BlockInputStreamPtr & prepared_input)
{
    /** Streams of data. When the query is executed in parallel, we have several data streams.
     *  If there is no GROUP BY, then perform all operations before ORDER BY and LIMIT in parallel, then
     *  if there is an ORDER BY, then glue the streams using UnionBlockInputStream, and then MergeSortingBlockInputStream,
     *  if not, then glue it using UnionBlockInputStream,
     *  then apply LIMIT.
     *  If there is GROUP BY, then we will perform all operations up to GROUP BY, inclusive, in parallel;
     *  a parallel GROUP BY will glue streams into one,
     *  then perform the remaining operations with one resulting stream.
     */

    constexpr bool pipeline_with_processors = std::is_same<TPipeline, QueryPipeline>::value;

    /// Now we will compose block streams that perform the necessary actions.
    auto & query = getSelectQuery();
    const Settings & settings = context.getSettingsRef();

    QueryProcessingStage::Enum from_stage = QueryProcessingStage::FetchColumns;

    /// PREWHERE optimization
    /// Turn off, if the table filter is applied.
    if (storage && !context.hasUserProperty(storage->getDatabaseName(), storage->getTableName(), "filter"))
    {
        if (!options.only_analyze)
            from_stage = storage->getQueryProcessingStage(context);

        query_analyzer->makeSetsForIndex(query.where());
        query_analyzer->makeSetsForIndex(query.prewhere());

        auto optimize_prewhere = [&](auto & merge_tree)
        {
            SelectQueryInfo current_info;
            current_info.query = query_ptr;
            current_info.syntax_analyzer_result = syntax_analyzer_result;
            current_info.sets = query_analyzer->getPreparedSets();

            /// Try transferring some condition from WHERE to PREWHERE if enabled and viable
            if (settings.optimize_move_to_prewhere && query.where() && !query.prewhere() && !query.final())
                MergeTreeWhereOptimizer{current_info, context, merge_tree, syntax_analyzer_result->requiredSourceColumns(), log};
        };

        if (const MergeTreeData * merge_tree_data = dynamic_cast<const MergeTreeData *>(storage.get()))
            optimize_prewhere(*merge_tree_data);
    }

    AnalysisResult expressions;
    FilterInfoPtr filter_info;

    /// We need proper `source_header` for `NullBlockInputStream` in dry-run.
    if (storage && context.hasUserProperty(storage->getDatabaseName(), storage->getTableName(), "filter"))
    {
        filter_info = std::make_shared<FilterInfo>();
        filter_info->column_name = generateFilterActions(filter_info->actions, storage, context, required_columns);
        source_header = storage->getSampleBlockForColumns(filter_info->actions->getRequiredColumns());
    }

    SortingInfoPtr sorting_info;
    if (settings.optimize_read_in_order && storage && query.orderBy() && !query_analyzer->hasAggregation() && !query.final() && !query.join())
    {
        if (const MergeTreeData * merge_tree_data = dynamic_cast<const MergeTreeData *>(storage.get()))
            sorting_info = optimizeReadInOrder(*merge_tree_data, query, context, syntax_analyzer_result);
    }

    if (options.only_analyze)
    {
        if constexpr (pipeline_with_processors)
            pipeline.init({std::make_shared<NullSource>(source_header)});
        else
            pipeline.streams.emplace_back(std::make_shared<NullBlockInputStream>(source_header));

        expressions = analyzeExpressions(
                getSelectQuery(),
                *query_analyzer,
                QueryProcessingStage::FetchColumns,
                options.to_stage,
                context,
                storage,
                true,
                filter_info);

        if (storage && expressions.filter_info && expressions.prewhere_info)
            throw Exception("PREWHERE is not supported if the table is filtered by row-level security expression", ErrorCodes::ILLEGAL_PREWHERE);

        if (expressions.prewhere_info)
        {
            if constexpr (pipeline_with_processors)
                pipeline.addSimpleTransform([&](const Block & header)
                {
                    return std::make_shared<FilterTransform>(
                            header,
                            expressions.prewhere_info->prewhere_actions,
                            expressions.prewhere_info->prewhere_column_name,
                            expressions.prewhere_info->remove_prewhere_column);
                });
            else
                pipeline.streams.back() = std::make_shared<FilterBlockInputStream>(
                    pipeline.streams.back(), expressions.prewhere_info->prewhere_actions,
                    expressions.prewhere_info->prewhere_column_name, expressions.prewhere_info->remove_prewhere_column);
        }
    }
    else
    {
        if (prepared_input)
        {
            if constexpr (pipeline_with_processors)
                pipeline.init({std::make_shared<SourceFromInputStream>(prepared_input)});
            else
                pipeline.streams.push_back(prepared_input);
        }

        expressions = analyzeExpressions(
                getSelectQuery(),
                *query_analyzer,
                from_stage,
                options.to_stage,
                context,
                storage,
                false,
                filter_info);

        if (from_stage == QueryProcessingStage::WithMergeableState &&
            options.to_stage == QueryProcessingStage::WithMergeableState)
            throw Exception("Distributed on Distributed is not supported", ErrorCodes::NOT_IMPLEMENTED);

        if (storage && expressions.filter_info && expressions.prewhere_info)
            throw Exception("PREWHERE is not supported if the table is filtered by row-level security expression", ErrorCodes::ILLEGAL_PREWHERE);

        /** Read the data from Storage. from_stage - to what stage the request was completed in Storage. */
        executeFetchColumns(from_stage, pipeline, sorting_info, expressions.prewhere_info, expressions.columns_to_remove_after_prewhere);

        LOG_TRACE(log, QueryProcessingStage::toString(from_stage) << " -> " << QueryProcessingStage::toString(options.to_stage));
    }

    if (options.to_stage > QueryProcessingStage::FetchColumns)
    {
        /// Do I need to aggregate in a separate row rows that have not passed max_rows_to_group_by.
        bool aggregate_overflow_row =
            expressions.need_aggregate &&
            query.group_by_with_totals &&
            settings.max_rows_to_group_by &&
            settings.group_by_overflow_mode == OverflowMode::ANY &&
            settings.totals_mode != TotalsMode::AFTER_HAVING_EXCLUSIVE;

        /// Do I need to immediately finalize the aggregate functions after the aggregation?
        bool aggregate_final =
            expressions.need_aggregate &&
            options.to_stage > QueryProcessingStage::WithMergeableState &&
            !query.group_by_with_totals && !query.group_by_with_rollup && !query.group_by_with_cube;

        if (expressions.first_stage)
        {
            if (expressions.filter_info)
            {
                if constexpr (pipeline_with_processors)
                {
                    pipeline.addSimpleTransform([&](const Block & block, QueryPipeline::StreamType stream_type) -> ProcessorPtr
                    {
                        if (stream_type == QueryPipeline::StreamType::Totals)
                            return nullptr;

                        return std::make_shared<FilterTransform>(
                            block,
                            expressions.filter_info->actions,
                            expressions.filter_info->column_name,
                            expressions.filter_info->do_remove_column);
                    });
                }
                else
                {
                    pipeline.transform([&](auto & stream)
                    {
                        stream = std::make_shared<FilterBlockInputStream>(
                            stream,
                            expressions.filter_info->actions,
                            expressions.filter_info->column_name,
                            expressions.filter_info->do_remove_column);
                    });
                }
            }

            if (expressions.hasJoin())
            {
                Block header_before_join;

                if constexpr (pipeline_with_processors)
                {
                    header_before_join = pipeline.getHeader();

                    /// In case joined subquery has totals, and we don't, add default chunk to totals.
                    bool default_totals = false;
                    if (!pipeline.hasTotals())
                    {
                        pipeline.addDefaultTotals();
                        default_totals = true;
                    }

                    pipeline.addSimpleTransform([&](const Block & header, QueryPipeline::StreamType type)
                    {
                        bool on_totals = type == QueryPipeline::StreamType::Totals;
                        return std::make_shared<ExpressionTransform>(header, expressions.before_join, on_totals, default_totals);
                    });
                }
                else
                {
                    header_before_join = pipeline.firstStream()->getHeader();
                    /// Applies to all sources except stream_with_non_joined_data.
                    for (auto & stream : pipeline.streams)
                        stream = std::make_shared<ExpressionBlockInputStream>(stream, expressions.before_join);
                }

                const auto & join = query.join()->table_join->as<ASTTableJoin &>();
                if (isRightOrFull(join.kind))
                {
                    auto stream = expressions.before_join->createStreamWithNonJoinedDataIfFullOrRightJoin(
                            header_before_join, settings.max_block_size);

                    if constexpr (pipeline_with_processors)
                    {
                        auto source = std::make_shared<SourceFromInputStream>(std::move(stream));
                        pipeline.addDelayedStream(source);
                    }
                    else
                        pipeline.stream_with_non_joined_data = std::move(stream);

                }
            }

            if (expressions.has_where)
                executeWhere(pipeline, expressions.before_where, expressions.remove_where_filter);

            if (expressions.need_aggregate)
                executeAggregation(pipeline, expressions.before_aggregation, aggregate_overflow_row, aggregate_final);
            else
            {
                executeExpression(pipeline, expressions.before_order_and_select);
                executeDistinct(pipeline, true, expressions.selected_columns);
            }

            /** For distributed query processing,
              *  if no GROUP, HAVING set,
              *  but there is an ORDER or LIMIT,
              *  then we will perform the preliminary sorting and LIMIT on the remote server.
              */
            if (!expressions.second_stage && !expressions.need_aggregate && !expressions.has_having)
            {
                if (expressions.has_order_by)
                    executeOrder(pipeline, query_info.sorting_info);

                if (expressions.has_order_by && query.limitLength())
                    executeDistinct(pipeline, false, expressions.selected_columns);

                if (expressions.has_limit_by)
                {
                    executeExpression(pipeline, expressions.before_limit_by);
                    executeLimitBy(pipeline);
                }

                if (query.limitLength())
                    executePreLimit(pipeline);
            }

            // If there is no global subqueries, we can run subqueries only when receive them on server.
            if (!query_analyzer->hasGlobalSubqueries() && !expressions.subqueries_for_sets.empty())
                executeSubqueriesInSetsAndJoins(pipeline, expressions.subqueries_for_sets);
        }

        if (expressions.second_stage)
        {
            bool need_second_distinct_pass = false;
            bool need_merge_streams = false;

            if (expressions.need_aggregate)
            {
                /// If you need to combine aggregated results from multiple servers
                if (!expressions.first_stage)
                    executeMergeAggregated(pipeline, aggregate_overflow_row, aggregate_final);

                if (!aggregate_final)
                {
                    if (query.group_by_with_totals)
                    {
                        bool final = !query.group_by_with_rollup && !query.group_by_with_cube;
                        executeTotalsAndHaving(pipeline, expressions.has_having, expressions.before_having, aggregate_overflow_row, final);
                    }

                    if (query.group_by_with_rollup)
                        executeRollupOrCube(pipeline, Modificator::ROLLUP);
                    else if (query.group_by_with_cube)
                        executeRollupOrCube(pipeline, Modificator::CUBE);

                    if ((query.group_by_with_rollup || query.group_by_with_cube) && expressions.has_having)
                    {
                        if (query.group_by_with_totals)
                            throw Exception("WITH TOTALS and WITH ROLLUP or CUBE are not supported together in presence of HAVING", ErrorCodes::NOT_IMPLEMENTED);
                        executeHaving(pipeline, expressions.before_having);
                    }
                }
                else if (expressions.has_having)
                    executeHaving(pipeline, expressions.before_having);

                executeExpression(pipeline, expressions.before_order_and_select);
                executeDistinct(pipeline, true, expressions.selected_columns);

                need_second_distinct_pass = query.distinct && pipeline.hasMixedStreams();
            }
            else
            {
                need_second_distinct_pass = query.distinct && pipeline.hasMixedStreams();

                if (query.group_by_with_totals && !aggregate_final)
                {
                    bool final = !query.group_by_with_rollup && !query.group_by_with_cube;
                    executeTotalsAndHaving(pipeline, expressions.has_having, expressions.before_having, aggregate_overflow_row, final);
                }

                if ((query.group_by_with_rollup || query.group_by_with_cube) && !aggregate_final)
                {
                    if (query.group_by_with_rollup)
                        executeRollupOrCube(pipeline, Modificator::ROLLUP);
                    else if (query.group_by_with_cube)
                        executeRollupOrCube(pipeline, Modificator::CUBE);

                    if (expressions.has_having)
                    {
                        if (query.group_by_with_totals)
                            throw Exception("WITH TOTALS and WITH ROLLUP or CUBE are not supported together in presence of HAVING", ErrorCodes::NOT_IMPLEMENTED);
                        executeHaving(pipeline, expressions.before_having);
                    }
                }
            }

            if (expressions.has_order_by)
            {
                /** If there is an ORDER BY for distributed query processing,
                  *  but there is no aggregation, then on the remote servers ORDER BY was made
                  *  - therefore, we merge the sorted streams from remote servers.
                  */

                if (!expressions.first_stage && !expressions.need_aggregate && !(query.group_by_with_totals && !aggregate_final))
                    executeMergeSorted(pipeline);
                else    /// Otherwise, just sort.
                    executeOrder(pipeline, query_info.sorting_info);
            }

            /** Optimization - if there are several sources and there is LIMIT, then first apply the preliminary LIMIT,
              * limiting the number of rows in each up to `offset + limit`.
              */
            if (query.limitLength() && pipeline.hasMoreThanOneStream() && !query.distinct && !expressions.has_limit_by && !settings.extremes)
            {
                executePreLimit(pipeline);
            }

            if (need_second_distinct_pass
                || query.limitLength()
                || query.limitBy()
                || pipeline.hasDelayedStream())
            {
                need_merge_streams = true;
            }

            if (need_merge_streams)
            {
                if constexpr (pipeline_with_processors)
                    pipeline.resize(1);
                else
                    executeUnion(pipeline, {});
            }

            /** If there was more than one stream,
              * then DISTINCT needs to be performed once again after merging all streams.
              */
            if (need_second_distinct_pass)
                executeDistinct(pipeline, false, expressions.selected_columns);

            if (expressions.has_limit_by)
            {
                executeExpression(pipeline, expressions.before_limit_by);
                executeLimitBy(pipeline);
            }

            /** We must do projection after DISTINCT because projection may remove some columns.
              */
            executeProjection(pipeline, expressions.final_projection);

            /** Extremes are calculated before LIMIT, but after LIMIT BY. This is Ok.
              */
            executeExtremes(pipeline);

            executeLimit(pipeline);
        }
    }

    if (query_analyzer->hasGlobalSubqueries() && !expressions.subqueries_for_sets.empty())
        executeSubqueriesInSetsAndJoins(pipeline, expressions.subqueries_for_sets);
}


template <typename TPipeline>
void InterpreterSelectQuery::executeFetchColumns(
        QueryProcessingStage::Enum processing_stage, TPipeline & pipeline,
        const SortingInfoPtr & sorting_info, const PrewhereInfoPtr & prewhere_info, const Names & columns_to_remove_after_prewhere)
{
    constexpr bool pipeline_with_processors = std::is_same<TPipeline, QueryPipeline>::value;

    auto & query = getSelectQuery();
    const Settings & settings = context.getSettingsRef();

    /// Actions to calculate ALIAS if required.
    ExpressionActionsPtr alias_actions;

    if (storage)
    {
        /// Append columns from the table filter to required
        if (context.hasUserProperty(storage->getDatabaseName(), storage->getTableName(), "filter"))
        {
            auto initial_required_columns = required_columns;
            ExpressionActionsPtr actions;
            generateFilterActions(actions, storage, context, initial_required_columns);
            auto required_columns_from_filter = actions->getRequiredColumns();

            for (const auto & column : required_columns_from_filter)
            {
                if (required_columns.end() == std::find(required_columns.begin(), required_columns.end(), column))
                    required_columns.push_back(column);
            }
        }

        /// Detect, if ALIAS columns are required for query execution
        auto alias_columns_required = false;
        const ColumnsDescription & storage_columns = storage->getColumns();
        for (const auto & column_name : required_columns)
        {
            auto column_default = storage_columns.getDefault(column_name);
            if (column_default && column_default->kind == ColumnDefaultKind::Alias)
            {
                alias_columns_required = true;
                break;
            }
        }

        /// There are multiple sources of required columns:
        ///  - raw required columns,
        ///  - columns deduced from ALIAS columns,
        ///  - raw required columns from PREWHERE,
        ///  - columns deduced from ALIAS columns from PREWHERE.
        /// PREWHERE is a special case, since we need to resolve it and pass directly to `IStorage::read()`
        /// before any other executions.
        if (alias_columns_required)
        {
            NameSet required_columns_from_prewhere; /// Set of all (including ALIAS) required columns for PREWHERE
            NameSet required_aliases_from_prewhere; /// Set of ALIAS required columns for PREWHERE

            if (prewhere_info)
            {
                /// Get some columns directly from PREWHERE expression actions
                auto prewhere_required_columns = prewhere_info->prewhere_actions->getRequiredColumns();
                required_columns_from_prewhere.insert(prewhere_required_columns.begin(), prewhere_required_columns.end());
            }

            /// Expression, that contains all raw required columns
            ASTPtr required_columns_all_expr = std::make_shared<ASTExpressionList>();

            /// Expression, that contains raw required columns for PREWHERE
            ASTPtr required_columns_from_prewhere_expr = std::make_shared<ASTExpressionList>();

            /// Sort out already known required columns between expressions,
            /// also populate `required_aliases_from_prewhere`.
            for (const auto & column : required_columns)
            {
                ASTPtr column_expr;
                const auto column_default = storage_columns.getDefault(column);
                bool is_alias = column_default && column_default->kind == ColumnDefaultKind::Alias;
                if (is_alias)
                    column_expr = setAlias(column_default->expression->clone(), column);
                else
                    column_expr = std::make_shared<ASTIdentifier>(column);

                if (required_columns_from_prewhere.count(column))
                {
                    required_columns_from_prewhere_expr->children.emplace_back(std::move(column_expr));

                    if (is_alias)
                        required_aliases_from_prewhere.insert(column);
                }
                else
                    required_columns_all_expr->children.emplace_back(std::move(column_expr));
            }

            /// Columns, which we will get after prewhere and filter executions.
            NamesAndTypesList required_columns_after_prewhere;
            NameSet required_columns_after_prewhere_set;

            /// Collect required columns from prewhere expression actions.
            if (prewhere_info)
            {
                NameSet columns_to_remove(columns_to_remove_after_prewhere.begin(), columns_to_remove_after_prewhere.end());
                Block prewhere_actions_result = prewhere_info->prewhere_actions->getSampleBlock();

                /// Populate required columns with the columns, added by PREWHERE actions and not removed afterwards.
                /// XXX: looks hacky that we already know which columns after PREWHERE we won't need for sure.
                for (const auto & column : prewhere_actions_result)
                {
                    if (prewhere_info->remove_prewhere_column && column.name == prewhere_info->prewhere_column_name)
                        continue;

                    if (columns_to_remove.count(column.name))
                        continue;

                    required_columns_all_expr->children.emplace_back(std::make_shared<ASTIdentifier>(column.name));
                    required_columns_after_prewhere.emplace_back(column.name, column.type);
                }

                required_columns_after_prewhere_set
                    = ext::map<NameSet>(required_columns_after_prewhere, [](const auto & it) { return it.name; });
            }

            auto syntax_result = SyntaxAnalyzer(context).analyze(required_columns_all_expr, required_columns_after_prewhere, {}, storage);
            alias_actions = ExpressionAnalyzer(required_columns_all_expr, syntax_result, context).getActions(true);

            /// The set of required columns could be added as a result of adding an action to calculate ALIAS.
            required_columns = alias_actions->getRequiredColumns();

            /// Do not remove prewhere filter if it is a column which is used as alias.
            if (prewhere_info && prewhere_info->remove_prewhere_column)
                if (required_columns.end()
                    != std::find(required_columns.begin(), required_columns.end(), prewhere_info->prewhere_column_name))
                    prewhere_info->remove_prewhere_column = false;

            /// Remove columns which will be added by prewhere.
            required_columns.erase(std::remove_if(required_columns.begin(), required_columns.end(), [&](const String & name)
            {
                return !!required_columns_after_prewhere_set.count(name);
            }), required_columns.end());

            if (prewhere_info)
            {
                /// Don't remove columns which are needed to be aliased.
                auto new_actions = std::make_shared<ExpressionActions>(prewhere_info->prewhere_actions->getRequiredColumnsWithTypes(), context);
                for (const auto & action : prewhere_info->prewhere_actions->getActions())
                {
                    if (action.type != ExpressionAction::REMOVE_COLUMN
                        || required_columns.end() == std::find(required_columns.begin(), required_columns.end(), action.source_name))
                        new_actions->add(action);
                }
                prewhere_info->prewhere_actions = std::move(new_actions);

                auto analyzed_result
                    = SyntaxAnalyzer(context).analyze(required_columns_from_prewhere_expr, storage->getColumns().getAllPhysical());
                prewhere_info->alias_actions
                    = ExpressionAnalyzer(required_columns_from_prewhere_expr, analyzed_result, context).getActions(true, false);

                /// Add (physical?) columns required by alias actions.
                auto required_columns_from_alias = prewhere_info->alias_actions->getRequiredColumns();
                Block prewhere_actions_result = prewhere_info->prewhere_actions->getSampleBlock();
                for (auto & column : required_columns_from_alias)
                    if (!prewhere_actions_result.has(column))
                        if (required_columns.end() == std::find(required_columns.begin(), required_columns.end(), column))
                            required_columns.push_back(column);

                /// Add physical columns required by prewhere actions.
                for (const auto & column : required_columns_from_prewhere)
                    if (required_aliases_from_prewhere.count(column) == 0)
                        if (required_columns.end() == std::find(required_columns.begin(), required_columns.end(), column))
                            required_columns.push_back(column);
            }
        }
    }

    /// Limitation on the number of columns to read.
    /// It's not applied in 'only_analyze' mode, because the query could be analyzed without removal of unnecessary columns.
    if (!options.only_analyze && settings.max_columns_to_read && required_columns.size() > settings.max_columns_to_read)
        throw Exception("Limit for number of columns to read exceeded. "
            "Requested: " + toString(required_columns.size())
            + ", maximum: " + settings.max_columns_to_read.toString(),
            ErrorCodes::TOO_MANY_COLUMNS);

    /** With distributed query processing, almost no computations are done in the threads,
     *  but wait and receive data from remote servers.
     *  If we have 20 remote servers, and max_threads = 8, then it would not be very good
     *  connect and ask only 8 servers at a time.
     *  To simultaneously query more remote servers,
     *  instead of max_threads, max_distributed_connections is used.
     */
    bool is_remote = false;
    if (storage && storage->isRemote())
    {
        is_remote = true;
        max_streams = settings.max_distributed_connections;
    }

    UInt64 max_block_size = settings.max_block_size;

    auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context);

    /** Optimization - if not specified DISTINCT, WHERE, GROUP, HAVING, ORDER, LIMIT BY but LIMIT is specified, and limit + offset < max_block_size,
     *  then as the block size we will use limit + offset (not to read more from the table than requested),
     *  and also set the number of threads to 1.
     */
    if (!query.distinct
        && !query.prewhere()
        && !query.where()
        && !query.groupBy()
        && !query.having()
        && !query.orderBy()
        && !query.limitBy()
        && query.limitLength()
        && !query_analyzer->hasAggregation()
        && limit_length + limit_offset < max_block_size)
    {
        max_block_size = std::max(UInt64(1), limit_length + limit_offset);
        max_streams = 1;
    }

    if (!max_block_size)
        throw Exception("Setting 'max_block_size' cannot be zero", ErrorCodes::PARAMETER_OUT_OF_BOUND);

    /// Initialize the initial data streams to which the query transforms are superimposed. Table or subquery or prepared input?
    if (pipeline.initialized())
    {
        /// Prepared input.
    }
    else if (interpreter_subquery)
    {
        /// Subquery.
        /// If we need less number of columns that subquery have - update the interpreter.
        if (required_columns.size() < source_header.columns())
        {
            ASTPtr subquery = extractTableExpression(query, 0);
            if (!subquery)
                throw Exception("Subquery expected", ErrorCodes::LOGICAL_ERROR);

            interpreter_subquery = std::make_unique<InterpreterSelectWithUnionQuery>(
                subquery, getSubqueryContext(context),
                options.copy().subquery().noModify(), required_columns);

            if (query_analyzer->hasAggregation())
                interpreter_subquery->ignoreWithTotals();
        }

        if constexpr (pipeline_with_processors)
            /// Just use pipeline from subquery.
            pipeline = interpreter_subquery->executeWithProcessors();
        else
            pipeline.streams = interpreter_subquery->executeWithMultipleStreams();
    }
    else if (storage)
    {
        /// Table.

        if (max_streams == 0)
            throw Exception("Logical error: zero number of streams requested", ErrorCodes::LOGICAL_ERROR);

        /// If necessary, we request more sources than the number of threads - to distribute the work evenly over the threads.
        if (max_streams > 1 && !is_remote)
            max_streams *= settings.max_streams_to_max_threads_ratio;

        query_info.query = query_ptr;
        query_info.syntax_analyzer_result = syntax_analyzer_result;
        query_info.sets = query_analyzer->getPreparedSets();
        query_info.prewhere_info = prewhere_info;
        query_info.sorting_info = sorting_info;

        auto streams = storage->read(required_columns, query_info, context, processing_stage, max_block_size, max_streams);

        if (streams.empty())
        {
            streams = {std::make_shared<NullBlockInputStream>(storage->getSampleBlockForColumns(required_columns))};

            if (query_info.prewhere_info)
                streams.back() = std::make_shared<FilterBlockInputStream>(
                    streams.back(),
                    prewhere_info->prewhere_actions,
                    prewhere_info->prewhere_column_name,
                    prewhere_info->remove_prewhere_column);

        }

        for (auto & stream : streams)
            stream->addTableLock(table_lock);

        /// Set the limits and quota for reading data, the speed and time of the query.
        {
            IBlockInputStream::LocalLimits limits;
            limits.mode = IBlockInputStream::LIMITS_TOTAL;
            limits.size_limits = SizeLimits(settings.max_rows_to_read, settings.max_bytes_to_read, settings.read_overflow_mode);
            limits.max_execution_time = settings.max_execution_time;
            limits.timeout_overflow_mode = settings.timeout_overflow_mode;

            /** Quota and minimal speed restrictions are checked on the initiating server of the request, and not on remote servers,
              *  because the initiating server has a summary of the execution of the request on all servers.
              *
              * But limits on data size to read and maximum execution time are reasonable to check both on initiator and
              *  additionally on each remote server, because these limits are checked per block of data processed,
              *  and remote servers may process way more blocks of data than are received by initiator.
              */
            if (options.to_stage == QueryProcessingStage::Complete)
            {
                limits.min_execution_speed = settings.min_execution_speed;
                limits.max_execution_speed = settings.max_execution_speed;
                limits.min_execution_speed_bytes = settings.min_execution_speed_bytes;
                limits.max_execution_speed_bytes = settings.max_execution_speed_bytes;
                limits.timeout_before_checking_execution_speed = settings.timeout_before_checking_execution_speed;
            }

            QuotaForIntervals & quota = context.getQuota();

            for (auto & stream : streams)
            {
                if (!options.ignore_limits)
                    stream->setLimits(limits);

                if (options.to_stage == QueryProcessingStage::Complete)
                    stream->setQuota(quota);
            }
        }

        if constexpr (pipeline_with_processors)
        {
            /// Unify streams. They must have same headers.
            if (streams.size() > 1)
            {
                /// Unify streams in case they have different headers.
                auto first_header = streams.at(0)->getHeader();

                if (first_header.columns() > 1 && first_header.has("_dummy"))
                    first_header.erase("_dummy");

                for (size_t i = 0; i < streams.size(); ++i)
                {
                    auto & stream = streams[i];
                    auto header = stream->getHeader();
                    auto mode = ConvertingBlockInputStream::MatchColumnsMode::Name;
                    if (!blocksHaveEqualStructure(first_header, header))
                        stream = std::make_shared<ConvertingBlockInputStream>(context, stream, first_header, mode);
                }
            }

            Processors sources;
            sources.reserve(streams.size());

            for (auto & stream : streams)
            {
                bool force_add_agg_info = processing_stage == QueryProcessingStage::WithMergeableState;
                auto source = std::make_shared<SourceFromInputStream>(stream, force_add_agg_info);

                if (processing_stage == QueryProcessingStage::Complete)
                    source->addTotalsPort();

                sources.emplace_back(std::move(source));

            }

            pipeline.init(std::move(sources));
        }
        else
            pipeline.streams = std::move(streams);
    }
    else
        throw Exception("Logical error in InterpreterSelectQuery: nowhere to read", ErrorCodes::LOGICAL_ERROR);

    /// Aliases in table declaration.
    if (processing_stage == QueryProcessingStage::FetchColumns && alias_actions)
    {
        if constexpr (pipeline_with_processors)
        {
            pipeline.addSimpleTransform([&](const Block & header)
            {
                return std::make_shared<ExpressionTransform>(header, alias_actions);
            });
        }
        else
        {
            pipeline.transform([&](auto & stream)
            {
                stream = std::make_shared<ExpressionBlockInputStream>(stream, alias_actions);
            });
        }
    }
}


void InterpreterSelectQuery::executeWhere(Pipeline & pipeline, const ExpressionActionsPtr & expression, bool remove_fiter)
{
    pipeline.transform([&](auto & stream)
    {
        stream = std::make_shared<FilterBlockInputStream>(stream, expression, getSelectQuery().where()->getColumnName(), remove_fiter);
    });
}

void InterpreterSelectQuery::executeWhere(QueryPipeline & pipeline, const ExpressionActionsPtr & expression, bool remove_fiter)
{
    pipeline.addSimpleTransform([&](const Block & block)
    {
        return std::make_shared<FilterTransform>(block, expression, getSelectQuery().where()->getColumnName(), remove_fiter);
    });
}

void InterpreterSelectQuery::executeAggregation(Pipeline & pipeline, const ExpressionActionsPtr & expression, bool overflow_row, bool final)
{
    pipeline.transform([&](auto & stream)
    {
        stream = std::make_shared<ExpressionBlockInputStream>(stream, expression);
    });

    Names key_names;
    AggregateDescriptions aggregates;
    query_analyzer->getAggregateInfo(key_names, aggregates);

    Block header = pipeline.firstStream()->getHeader();
    ColumnNumbers keys;
    for (const auto & name : key_names)
        keys.push_back(header.getPositionByName(name));
    for (auto & descr : aggregates)
        if (descr.arguments.empty())
            for (const auto & name : descr.argument_names)
                descr.arguments.push_back(header.getPositionByName(name));

    const Settings & settings = context.getSettingsRef();

    /** Two-level aggregation is useful in two cases:
      * 1. Parallel aggregation is done, and the results should be merged in parallel.
      * 2. An aggregation is done with store of temporary data on the disk, and they need to be merged in a memory efficient way.
      */
    bool allow_to_use_two_level_group_by = pipeline.streams.size() > 1 || settings.max_bytes_before_external_group_by != 0;

    Aggregator::Params params(header, keys, aggregates,
        overflow_row, settings.max_rows_to_group_by, settings.group_by_overflow_mode,
        settings.compile ? &context.getCompiler() : nullptr, settings.min_count_to_compile,
        allow_to_use_two_level_group_by ? settings.group_by_two_level_threshold : SettingUInt64(0),
        allow_to_use_two_level_group_by ? settings.group_by_two_level_threshold_bytes : SettingUInt64(0),
        settings.max_bytes_before_external_group_by, settings.empty_result_for_aggregation_by_empty_set,
        context.getTemporaryPath(), settings.max_threads);

    /// If there are several sources, then we perform parallel aggregation
    if (pipeline.streams.size() > 1)
    {
        pipeline.firstStream() = std::make_shared<ParallelAggregatingBlockInputStream>(
            pipeline.streams, pipeline.stream_with_non_joined_data, params, final,
            max_streams,
            settings.aggregation_memory_efficient_merge_threads
                ? static_cast<size_t>(settings.aggregation_memory_efficient_merge_threads)
                : static_cast<size_t>(settings.max_threads));

        pipeline.stream_with_non_joined_data = nullptr;
        pipeline.streams.resize(1);
    }
    else
    {
        BlockInputStreams inputs;
        if (!pipeline.streams.empty())
            inputs.push_back(pipeline.firstStream());
        else
            pipeline.streams.resize(1);

        if (pipeline.stream_with_non_joined_data)
            inputs.push_back(pipeline.stream_with_non_joined_data);

        pipeline.firstStream() = std::make_shared<AggregatingBlockInputStream>(std::make_shared<ConcatBlockInputStream>(inputs), params, final);

        pipeline.stream_with_non_joined_data = nullptr;
    }
}


void InterpreterSelectQuery::executeAggregation(QueryPipeline & pipeline, const ExpressionActionsPtr & expression, bool overflow_row, bool final)
{
    pipeline.addSimpleTransform([&](const Block & header)
    {
        return std::make_shared<ExpressionTransform>(header, expression);
    });

    Names key_names;
    AggregateDescriptions aggregates;
    query_analyzer->getAggregateInfo(key_names, aggregates);

    Block header_before_aggregation = pipeline.getHeader();
    ColumnNumbers keys;
    for (const auto & name : key_names)
        keys.push_back(header_before_aggregation.getPositionByName(name));
    for (auto & descr : aggregates)
        if (descr.arguments.empty())
            for (const auto & name : descr.argument_names)
                descr.arguments.push_back(header_before_aggregation.getPositionByName(name));

    const Settings & settings = context.getSettingsRef();

    /** Two-level aggregation is useful in two cases:
      * 1. Parallel aggregation is done, and the results should be merged in parallel.
      * 2. An aggregation is done with store of temporary data on the disk, and they need to be merged in a memory efficient way.
      */
    bool allow_to_use_two_level_group_by = pipeline.getNumMainStreams() > 1 || settings.max_bytes_before_external_group_by != 0;

    Aggregator::Params params(header_before_aggregation, keys, aggregates,
                              overflow_row, settings.max_rows_to_group_by, settings.group_by_overflow_mode,
                              settings.compile ? &context.getCompiler() : nullptr, settings.min_count_to_compile,
                              allow_to_use_two_level_group_by ? settings.group_by_two_level_threshold : SettingUInt64(0),
                              allow_to_use_two_level_group_by ? settings.group_by_two_level_threshold_bytes : SettingUInt64(0),
                              settings.max_bytes_before_external_group_by, settings.empty_result_for_aggregation_by_empty_set,
                              context.getTemporaryPath(), settings.max_threads);

    auto transform_params = std::make_shared<AggregatingTransformParams>(params, final);

    pipeline.dropTotalsIfHas();

    /// If there are several sources, then we perform parallel aggregation
    if (pipeline.getNumMainStreams() > 1)
    {
        pipeline.resize(max_streams);

        auto many_data = std::make_shared<ManyAggregatedData>(max_streams);
        auto merge_threads = settings.aggregation_memory_efficient_merge_threads
                ? static_cast<size_t>(settings.aggregation_memory_efficient_merge_threads)
                : static_cast<size_t>(settings.max_threads);

        size_t counter = 0;
        pipeline.addSimpleTransform([&](const Block & header)
        {
            return std::make_shared<AggregatingTransform>(header, transform_params, many_data, counter++, max_streams, merge_threads);
        });

        pipeline.resize(1);
    }
    else
    {
        pipeline.resize(1);

        pipeline.addSimpleTransform([&](const Block & header)
        {
            return std::make_shared<AggregatingTransform>(header, transform_params);
        });
    }
}


void InterpreterSelectQuery::executeMergeAggregated(Pipeline & pipeline, bool overflow_row, bool final)
{
    Names key_names;
    AggregateDescriptions aggregates;
    query_analyzer->getAggregateInfo(key_names, aggregates);

    Block header = pipeline.firstStream()->getHeader();

    ColumnNumbers keys;
    for (const auto & name : key_names)
        keys.push_back(header.getPositionByName(name));

    /** There are two modes of distributed aggregation.
      *
      * 1. In different threads read from the remote servers blocks.
      * Save all the blocks in the RAM. Merge blocks.
      * If the aggregation is two-level - parallelize to the number of buckets.
      *
      * 2. In one thread, read blocks from different servers in order.
      * RAM stores only one block from each server.
      * If the aggregation is a two-level aggregation, we consistently merge the blocks of each next level.
      *
      * The second option consumes less memory (up to 256 times less)
      *  in the case of two-level aggregation, which is used for large results after GROUP BY,
      *  but it can work more slowly.
      */

    const Settings & settings = context.getSettingsRef();

    Aggregator::Params params(header, keys, aggregates, overflow_row, settings.max_threads);

    if (!settings.distributed_aggregation_memory_efficient)
    {
        /// We union several sources into one, parallelizing the work.
        executeUnion(pipeline, {});

        /// Now merge the aggregated blocks
        pipeline.firstStream() = std::make_shared<MergingAggregatedBlockInputStream>(pipeline.firstStream(), params, final, settings.max_threads);
    }
    else
    {
        pipeline.firstStream() = std::make_shared<MergingAggregatedMemoryEfficientBlockInputStream>(pipeline.streams, params, final,
            max_streams,
            settings.aggregation_memory_efficient_merge_threads
                ? static_cast<size_t>(settings.aggregation_memory_efficient_merge_threads)
                : static_cast<size_t>(settings.max_threads));

        pipeline.streams.resize(1);
    }
}

void InterpreterSelectQuery::executeMergeAggregated(QueryPipeline & pipeline, bool overflow_row, bool final)
{
    Names key_names;
    AggregateDescriptions aggregates;
    query_analyzer->getAggregateInfo(key_names, aggregates);

    Block header_before_merge = pipeline.getHeader();

    ColumnNumbers keys;
    for (const auto & name : key_names)
        keys.push_back(header_before_merge.getPositionByName(name));

    /** There are two modes of distributed aggregation.
      *
      * 1. In different threads read from the remote servers blocks.
      * Save all the blocks in the RAM. Merge blocks.
      * If the aggregation is two-level - parallelize to the number of buckets.
      *
      * 2. In one thread, read blocks from different servers in order.
      * RAM stores only one block from each server.
      * If the aggregation is a two-level aggregation, we consistently merge the blocks of each next level.
      *
      * The second option consumes less memory (up to 256 times less)
      *  in the case of two-level aggregation, which is used for large results after GROUP BY,
      *  but it can work more slowly.
      */

    const Settings & settings = context.getSettingsRef();

    Aggregator::Params params(header_before_merge, keys, aggregates, overflow_row, settings.max_threads);

    auto transform_params = std::make_shared<AggregatingTransformParams>(params, final);

    if (!settings.distributed_aggregation_memory_efficient)
    {
        /// We union several sources into one, parallelizing the work.
        pipeline.resize(1);

        /// Now merge the aggregated blocks
        pipeline.addSimpleTransform([&](const Block & header)
        {
            return std::make_shared<MergingAggregatedTransform>(header, transform_params, settings.max_threads);
        });
    }
    else
    {
        /// pipeline.resize(max_streams); - Seem we don't need it.
        auto num_merge_threads = settings.aggregation_memory_efficient_merge_threads
                                 ? static_cast<size_t>(settings.aggregation_memory_efficient_merge_threads)
                                 : static_cast<size_t>(settings.max_threads);

        auto pipe = createMergingAggregatedMemoryEfficientPipe(
            pipeline.getHeader(),
            transform_params,
            pipeline.getNumStreams(),
            num_merge_threads);

        pipeline.addPipe(std::move(pipe));
    }
}


void InterpreterSelectQuery::executeHaving(Pipeline & pipeline, const ExpressionActionsPtr & expression)
{
    pipeline.transform([&](auto & stream)
    {
        stream = std::make_shared<FilterBlockInputStream>(stream, expression, getSelectQuery().having()->getColumnName());
    });
}

void InterpreterSelectQuery::executeHaving(QueryPipeline & pipeline, const ExpressionActionsPtr & expression)
{
    pipeline.addSimpleTransform([&](const Block & header, QueryPipeline::StreamType stream_type) -> ProcessorPtr
    {
        if (stream_type == QueryPipeline::StreamType::Totals)
            return nullptr;

        /// TODO: do we need to save filter there?
        return std::make_shared<FilterTransform>(header, expression, getSelectQuery().having()->getColumnName(), false);
    });
}


void InterpreterSelectQuery::executeTotalsAndHaving(Pipeline & pipeline, bool has_having, const ExpressionActionsPtr & expression, bool overflow_row, bool final)
{
    executeUnion(pipeline, {});

    const Settings & settings = context.getSettingsRef();

    pipeline.firstStream() = std::make_shared<TotalsHavingBlockInputStream>(
        pipeline.firstStream(),
        overflow_row,
        expression,
        has_having ? getSelectQuery().having()->getColumnName() : "",
        settings.totals_mode,
        settings.totals_auto_threshold,
        final);
}

void InterpreterSelectQuery::executeTotalsAndHaving(QueryPipeline & pipeline, bool has_having, const ExpressionActionsPtr & expression, bool overflow_row, bool final)
{
    const Settings & settings = context.getSettingsRef();

    auto totals_having = std::make_shared<TotalsHavingTransform>(
            pipeline.getHeader(), overflow_row, expression,
            has_having ? getSelectQuery().having()->getColumnName() : "",
            settings.totals_mode, settings.totals_auto_threshold, final);

    pipeline.addTotalsHavingTransform(std::move(totals_having));
}


void InterpreterSelectQuery::executeRollupOrCube(Pipeline & pipeline, Modificator modificator)
{
    executeUnion(pipeline, {});

    Names key_names;
    AggregateDescriptions aggregates;
    query_analyzer->getAggregateInfo(key_names, aggregates);

    Block header = pipeline.firstStream()->getHeader();

    ColumnNumbers keys;

    for (const auto & name : key_names)
        keys.push_back(header.getPositionByName(name));

    const Settings & settings = context.getSettingsRef();

    Aggregator::Params params(header, keys, aggregates,
        false, settings.max_rows_to_group_by, settings.group_by_overflow_mode,
        settings.compile ? &context.getCompiler() : nullptr, settings.min_count_to_compile,
        SettingUInt64(0), SettingUInt64(0),
        settings.max_bytes_before_external_group_by, settings.empty_result_for_aggregation_by_empty_set,
        context.getTemporaryPath(), settings.max_threads);

    if (modificator == Modificator::ROLLUP)
        pipeline.firstStream() = std::make_shared<RollupBlockInputStream>(pipeline.firstStream(), params);
    else
        pipeline.firstStream() = std::make_shared<CubeBlockInputStream>(pipeline.firstStream(), params);
}

void InterpreterSelectQuery::executeRollupOrCube(QueryPipeline & pipeline, Modificator modificator)
{
    pipeline.resize(1);

    Names key_names;
    AggregateDescriptions aggregates;
    query_analyzer->getAggregateInfo(key_names, aggregates);

    Block header_before_transform = pipeline.getHeader();

    ColumnNumbers keys;

    for (const auto & name : key_names)
        keys.push_back(header_before_transform.getPositionByName(name));

    const Settings & settings = context.getSettingsRef();

    Aggregator::Params params(header_before_transform, keys, aggregates,
                              false, settings.max_rows_to_group_by, settings.group_by_overflow_mode,
                              settings.compile ? &context.getCompiler() : nullptr, settings.min_count_to_compile,
                              SettingUInt64(0), SettingUInt64(0),
                              settings.max_bytes_before_external_group_by, settings.empty_result_for_aggregation_by_empty_set,
                              context.getTemporaryPath(), settings.max_threads);

    auto transform_params = std::make_shared<AggregatingTransformParams>(params, true);

    pipeline.addSimpleTransform([&](const Block & header, QueryPipeline::StreamType stream_type) -> ProcessorPtr
    {
        if (stream_type == QueryPipeline::StreamType::Totals)
            return nullptr;

        if (modificator == Modificator::ROLLUP)
            return std::make_shared<RollupTransform>(header, std::move(transform_params));
        else
            return std::make_shared<CubeTransform>(header, std::move(transform_params));
    });
}


void InterpreterSelectQuery::executeExpression(Pipeline & pipeline, const ExpressionActionsPtr & expression)
{
    pipeline.transform([&](auto & stream)
    {
        stream = std::make_shared<ExpressionBlockInputStream>(stream, expression);
    });
}

void InterpreterSelectQuery::executeExpression(QueryPipeline & pipeline, const ExpressionActionsPtr & expression)
{
    pipeline.addSimpleTransform([&](const Block & header) -> ProcessorPtr
    {
        return std::make_shared<ExpressionTransform>(header, expression);
    });
}

void InterpreterSelectQuery::executeOrder(Pipeline & pipeline, SortingInfoPtr sorting_info)
{
    auto & query = getSelectQuery();
    SortDescription order_descr = getSortDescription(query);
    const Settings & settings = context.getSettingsRef();
    UInt64 limit = getLimitForSorting(query, context);

    if (sorting_info)
    {
        /* Case of sorting with optimization using sorting key.
         * We have several threads, each of them reads batch of parts in direct
         *  or reverse order of sorting key using one input stream per part
         *  and then merge them into one sorted stream.
         * At this stage we merge per-thread streams into one.
         */

        bool need_finish_sorting = (sorting_info->prefix_order_descr.size() < order_descr.size());
        if (need_finish_sorting)
        {
            pipeline.transform([&](auto & stream)
            {
                stream = std::make_shared<PartialSortingBlockInputStream>(stream, order_descr, limit);
            });
        }

        if (pipeline.hasMoreThanOneStream())
        {
            pipeline.transform([&](auto & stream)
            {
                stream = std::make_shared<AsynchronousBlockInputStream>(stream);
            });

            UInt64 limit_for_merging = (need_finish_sorting ? 0 : limit);
            pipeline.firstStream() = std::make_shared<MergingSortedBlockInputStream>(
                pipeline.streams, sorting_info->prefix_order_descr,
                settings.max_block_size, limit_for_merging);
            pipeline.streams.resize(1);
        }

        if (need_finish_sorting)
        {
            pipeline.firstStream() = std::make_shared<FinishSortingBlockInputStream>(
                pipeline.firstStream(), sorting_info->prefix_order_descr,
                order_descr, settings.max_block_size, limit);
        }
    }
    else
    {
        pipeline.transform([&](auto & stream)
        {
            auto sorting_stream = std::make_shared<PartialSortingBlockInputStream>(stream, order_descr, limit);

            /// Limits on sorting
            IBlockInputStream::LocalLimits limits;
            limits.mode = IBlockInputStream::LIMITS_TOTAL;
            limits.size_limits = SizeLimits(settings.max_rows_to_sort, settings.max_bytes_to_sort, settings.sort_overflow_mode);
            sorting_stream->setLimits(limits);

            stream = sorting_stream;
        });

        /// If there are several streams, we merge them into one
        executeUnion(pipeline, {});

        /// Merge the sorted blocks.
        pipeline.firstStream() = std::make_shared<MergeSortingBlockInputStream>(
            pipeline.firstStream(), order_descr, settings.max_block_size, limit,
            settings.max_bytes_before_remerge_sort,
            settings.max_bytes_before_external_sort, context.getTemporaryPath());
    }
}

void InterpreterSelectQuery::executeOrder(QueryPipeline & pipeline, SortingInfoPtr /* sorting_info */)
{
    /// TODO: Implement optimization using sorting_info

    auto & query = getSelectQuery();
    SortDescription order_descr = getSortDescription(query);
    UInt64 limit = getLimitForSorting(query, context);

    const Settings & settings = context.getSettingsRef();

    /// TODO: Limits on sorting
//    IBlockInputStream::LocalLimits limits;
//    limits.mode = IBlockInputStream::LIMITS_TOTAL;
//    limits.size_limits = SizeLimits(settings.max_rows_to_sort, settings.max_bytes_to_sort, settings.sort_overflow_mode);


    pipeline.addSimpleTransform([&](const Block & header, QueryPipeline::StreamType stream_type)
    {
        bool do_count_rows = stream_type == QueryPipeline::StreamType::Main;
        return std::make_shared<PartialSortingTransform>(header, order_descr, limit, do_count_rows);
    });

    /// If there are several streams, we merge them into one
    pipeline.resize(1);

    /// Merge the sorted blocks.
    pipeline.addSimpleTransform([&](const Block & header, QueryPipeline::StreamType stream_type) -> ProcessorPtr
    {
        if (stream_type == QueryPipeline::StreamType::Totals)
            return nullptr;

        return std::make_shared<MergeSortingTransform>(
                header, order_descr, settings.max_block_size, limit,
                settings.max_bytes_before_remerge_sort,
                settings.max_bytes_before_external_sort, context.getTemporaryPath());
    });
}


void InterpreterSelectQuery::executeMergeSorted(Pipeline & pipeline)
{
    auto & query = getSelectQuery();
    SortDescription order_descr = getSortDescription(query);
    UInt64 limit = getLimitForSorting(query, context);

    const Settings & settings = context.getSettingsRef();

    /// If there are several streams, then we merge them into one
    if (pipeline.hasMoreThanOneStream())
    {
        unifyStreams(pipeline, pipeline.firstStream()->getHeader());

        /** MergingSortedBlockInputStream reads the sources sequentially.
          * To make the data on the remote servers prepared in parallel, we wrap it in AsynchronousBlockInputStream.
          */
        pipeline.transform([&](auto & stream)
        {
            stream = std::make_shared<AsynchronousBlockInputStream>(stream);
        });

        /// Merge the sorted sources into one sorted source.
        pipeline.firstStream() = std::make_shared<MergingSortedBlockInputStream>(pipeline.streams, order_descr, settings.max_block_size, limit);
        pipeline.streams.resize(1);
    }
}

void InterpreterSelectQuery::executeMergeSorted(QueryPipeline & pipeline)
{
    auto & query = getSelectQuery();
    SortDescription order_descr = getSortDescription(query);
    UInt64 limit = getLimitForSorting(query, context);

    const Settings & settings = context.getSettingsRef();

    /// If there are several streams, then we merge them into one
    if (pipeline.getNumStreams() > 1)
    {
        auto transform = std::make_shared<MergingSortedTransform>(
            pipeline.getHeader(),
            pipeline.getNumStreams(),
            order_descr,
            settings.max_block_size, limit);

        pipeline.addPipe({ std::move(transform) });
    }
}


void InterpreterSelectQuery::executeProjection(Pipeline & pipeline, const ExpressionActionsPtr & expression)
{
    pipeline.transform([&](auto & stream)
    {
        stream = std::make_shared<ExpressionBlockInputStream>(stream, expression);
    });
}

void InterpreterSelectQuery::executeProjection(QueryPipeline & pipeline, const ExpressionActionsPtr & expression)
{
    pipeline.addSimpleTransform([&](const Block & header) -> ProcessorPtr
    {
       return std::make_shared<ExpressionTransform>(header, expression);
    });
}


void InterpreterSelectQuery::executeDistinct(Pipeline & pipeline, bool before_order, Names columns)
{
    auto & query = getSelectQuery();
    if (query.distinct)
    {
        const Settings & settings = context.getSettingsRef();

        auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context);
        UInt64 limit_for_distinct = 0;

        /// If after this stage of DISTINCT ORDER BY is not executed, then you can get no more than limit_length + limit_offset of different rows.
        if (!query.orderBy() || !before_order)
            limit_for_distinct = limit_length + limit_offset;

        pipeline.transform([&](auto & stream)
        {
            SizeLimits limits(settings.max_rows_in_distinct, settings.max_bytes_in_distinct, settings.distinct_overflow_mode);
            stream = std::make_shared<DistinctBlockInputStream>(stream, limits, limit_for_distinct, columns);
        });
    }
}

void InterpreterSelectQuery::executeDistinct(QueryPipeline & pipeline, bool before_order, Names columns)
{
    auto & query = getSelectQuery();
    if (query.distinct)
    {
        const Settings & settings = context.getSettingsRef();

        auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context);
        UInt64 limit_for_distinct = 0;

        /// If after this stage of DISTINCT ORDER BY is not executed, then you can get no more than limit_length + limit_offset of different rows.
        if (!query.orderBy() || !before_order)
            limit_for_distinct = limit_length + limit_offset;

        SizeLimits limits(settings.max_rows_in_distinct, settings.max_bytes_in_distinct, settings.distinct_overflow_mode);

        pipeline.addSimpleTransform([&](const Block & header, QueryPipeline::StreamType stream_type) -> ProcessorPtr
        {
            if (stream_type == QueryPipeline::StreamType::Totals)
                return nullptr;

            return std::make_shared<DistinctTransform>(header, limits, limit_for_distinct, columns);
        });
    }
}


void InterpreterSelectQuery::executeUnion(Pipeline & pipeline, Block header)
{
    /// If there are still several streams, then we combine them into one
    if (pipeline.hasMoreThanOneStream())
    {
        if (!header)
            header = pipeline.firstStream()->getHeader();

        unifyStreams(pipeline, std::move(header));

        pipeline.firstStream() = std::make_shared<UnionBlockInputStream>(pipeline.streams, pipeline.stream_with_non_joined_data, max_streams);
        pipeline.stream_with_non_joined_data = nullptr;
        pipeline.streams.resize(1);
        pipeline.union_stream = true;
    }
    else if (pipeline.stream_with_non_joined_data)
    {
        pipeline.streams.push_back(pipeline.stream_with_non_joined_data);
        pipeline.stream_with_non_joined_data = nullptr;
    }
}


/// Preliminary LIMIT - is used in every source, if there are several sources, before they are combined.
void InterpreterSelectQuery::executePreLimit(Pipeline & pipeline)
{
    auto & query = getSelectQuery();
    /// If there is LIMIT
    if (query.limitLength())
    {
        auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context);
        pipeline.transform([&, limit = limit_length + limit_offset](auto & stream)
        {
            stream = std::make_shared<LimitBlockInputStream>(stream, limit, 0, false);
        });
    }
}

/// Preliminary LIMIT - is used in every source, if there are several sources, before they are combined.
void InterpreterSelectQuery::executePreLimit(QueryPipeline & pipeline)
{
    auto & query = getSelectQuery();
    /// If there is LIMIT
    if (query.limitLength())
    {
        auto [limit_length, limit_offset] = getLimitLengthAndOffset(query, context);
        pipeline.addSimpleTransform([&, limit = limit_length + limit_offset](const Block & header, QueryPipeline::StreamType stream_type) -> ProcessorPtr
        {
            if (stream_type == QueryPipeline::StreamType::Totals)
                return nullptr;

            return std::make_shared<LimitTransform>(header, limit, 0);
        });
    }
}


void InterpreterSelectQuery::executeLimitBy(Pipeline & pipeline)
{
    auto & query = getSelectQuery();
    if (!query.limitByLength() || !query.limitBy())
        return;

    Names columns;
    for (const auto & elem : query.limitBy()->children)
        columns.emplace_back(elem->getColumnName());
    UInt64 length = getLimitUIntValue(query.limitByLength(), context);
    UInt64 offset = (query.limitByOffset() ? getLimitUIntValue(query.limitByOffset(), context) : 0);

    pipeline.transform([&](auto & stream)
    {
        stream = std::make_shared<LimitByBlockInputStream>(stream, length, offset, columns);
    });
}

void InterpreterSelectQuery::executeLimitBy(QueryPipeline & pipeline)
{
    auto & query = getSelectQuery();
    if (!query.limitByLength() || !query.limitBy())
        return;

    Names columns;
    for (const auto & elem : query.limitBy()->children)
        columns.emplace_back(elem->getColumnName());

    UInt64 length = getLimitUIntValue(query.limitByLength(), context);
    UInt64 offset = (query.limitByOffset() ? getLimitUIntValue(query.limitByOffset(), context) : 0);

    pipeline.addSimpleTransform([&](const Block & header, QueryPipeline::StreamType stream_type) -> ProcessorPtr
    {
        if (stream_type == QueryPipeline::StreamType::Totals)
            return nullptr;

        return std::make_shared<LimitByTransform>(header, length, offset, columns);
    });
}


// TODO: move to anonymous namespace
bool hasWithTotalsInAnySubqueryInFromClause(const ASTSelectQuery & query)
{
    if (query.group_by_with_totals)
        return true;

    /** NOTE You can also check that the table in the subquery is distributed, and that it only looks at one shard.
      * In other cases, totals will be computed on the initiating server of the query, and it is not necessary to read the data to the end.
      */

    if (auto query_table = extractTableExpression(query, 0))
    {
        if (const auto * ast_union = query_table->as<ASTSelectWithUnionQuery>())
        {
            for (const auto & elem : ast_union->list_of_selects->children)
                if (hasWithTotalsInAnySubqueryInFromClause(elem->as<ASTSelectQuery &>()))
                    return true;
        }
    }

    return false;
}


void InterpreterSelectQuery::executeLimit(Pipeline & pipeline)
{
    auto & query = getSelectQuery();
    /// If there is LIMIT
    if (query.limitLength())
    {
        /** Rare case:
          *  if there is no WITH TOTALS and there is a subquery in FROM, and there is WITH TOTALS on one of the levels,
          *  then when using LIMIT, you should read the data to the end, rather than cancel the query earlier,
          *  because if you cancel the query, we will not get `totals` data from the remote server.
          *
          * Another case:
          *  if there is WITH TOTALS and there is no ORDER BY, then read the data to the end,
          *  otherwise TOTALS is counted according to incomplete data.
          */
        bool always_read_till_end = false;

        if (query.group_by_with_totals && !query.orderBy())
            always_read_till_end = true;

        if (!query.group_by_with_totals && hasWithTotalsInAnySubqueryInFromClause(query))
            always_read_till_end = true;

        UInt64 limit_length;
        UInt64 limit_offset;
        std::tie(limit_length, limit_offset) = getLimitLengthAndOffset(query, context);

        pipeline.transform([&](auto & stream)
        {
            stream = std::make_shared<LimitBlockInputStream>(stream, limit_length, limit_offset, always_read_till_end);
        });
    }
}

void InterpreterSelectQuery::executeLimit(QueryPipeline & pipeline)
{
    auto & query = getSelectQuery();
    /// If there is LIMIT
    if (query.limitLength())
    {
        /** Rare case:
          *  if there is no WITH TOTALS and there is a subquery in FROM, and there is WITH TOTALS on one of the levels,
          *  then when using LIMIT, you should read the data to the end, rather than cancel the query earlier,
          *  because if you cancel the query, we will not get `totals` data from the remote server.
          *
          * Another case:
          *  if there is WITH TOTALS and there is no ORDER BY, then read the data to the end,
          *  otherwise TOTALS is counted according to incomplete data.
          */
        bool always_read_till_end = false;

        if (query.group_by_with_totals && !query.orderBy())
            always_read_till_end = true;

        if (!query.group_by_with_totals && hasWithTotalsInAnySubqueryInFromClause(query))
            always_read_till_end = true;

        UInt64 limit_length;
        UInt64 limit_offset;
        std::tie(limit_length, limit_offset) = getLimitLengthAndOffset(query, context);

        pipeline.addSimpleTransform([&](const Block & header, QueryPipeline::StreamType stream_type) -> ProcessorPtr
        {
            if (stream_type != QueryPipeline::StreamType::Main)
                return nullptr;

            return std::make_shared<LimitTransform>(
                    header, limit_length, limit_offset, always_read_till_end);
        });
    }
}


void InterpreterSelectQuery::executeExtremes(Pipeline & pipeline)
{
    if (!context.getSettingsRef().extremes)
        return;

    pipeline.transform([&](auto & stream)
    {
        stream->enableExtremes();
    });
}

void InterpreterSelectQuery::executeExtremes(QueryPipeline & pipeline)
{
    if (!context.getSettingsRef().extremes)
        return;

    auto transform = std::make_shared<ExtremesTransform>(pipeline.getHeader());
    pipeline.addExtremesTransform(std::move(transform));
}


void InterpreterSelectQuery::executeSubqueriesInSetsAndJoins(Pipeline & pipeline, SubqueriesForSets & subqueries_for_sets)
{
    executeUnion(pipeline, {});
    pipeline.firstStream() = std::make_shared<CreatingSetsBlockInputStream>(
        pipeline.firstStream(), subqueries_for_sets, context);
}

void InterpreterSelectQuery::executeSubqueriesInSetsAndJoins(QueryPipeline & pipeline, SubqueriesForSets & subqueries_for_sets)
{
    const Settings & settings = context.getSettingsRef();

    auto creating_sets = std::make_shared<CreatingSetsTransform>(
            pipeline.getHeader(), subqueries_for_sets,
            SizeLimits(settings.max_rows_to_transfer, settings.max_bytes_to_transfer, settings.transfer_overflow_mode),
            context);

    pipeline.addCreatingSetsTransform(std::move(creating_sets));
}


void InterpreterSelectQuery::unifyStreams(Pipeline & pipeline, Block header)
{
    /// Unify streams in case they have different headers.

    /// TODO: remove previos addition of _dummy column.
    if (header.columns() > 1 && header.has("_dummy"))
        header.erase("_dummy");

    for (size_t i = 0; i < pipeline.streams.size(); ++i)
    {
        auto & stream = pipeline.streams[i];
        auto stream_header = stream->getHeader();
        auto mode = ConvertingBlockInputStream::MatchColumnsMode::Name;

        if (!blocksHaveEqualStructure(header, stream_header))
            stream = std::make_shared<ConvertingBlockInputStream>(context, stream, header, mode);
    }
}


void InterpreterSelectQuery::ignoreWithTotals()
{
    getSelectQuery().group_by_with_totals = false;
}


void InterpreterSelectQuery::initSettings()
{
    auto & query = getSelectQuery();
    if (query.settings())
        InterpreterSetQuery(query.settings(), context).executeForCurrentContext();
}

}
