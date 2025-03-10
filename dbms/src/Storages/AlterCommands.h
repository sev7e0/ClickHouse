#pragma once

#include <optional>
#include <Core/NamesAndTypes.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/IStorage_fwd.h>
#include <Storages/IndicesDescription.h>


namespace DB
{

class ASTAlterCommand;

/// Operation from the ALTER query (except for manipulation with PART/PARTITION).
/// Adding Nested columns is not expanded to add individual columns.
struct AlterCommand
{
    enum Type
    {
        ADD_COLUMN,
        DROP_COLUMN,
        MODIFY_COLUMN,
        COMMENT_COLUMN,
        MODIFY_ORDER_BY,
        ADD_INDEX,
        DROP_INDEX,
        MODIFY_TTL,
        UKNOWN_TYPE,
    };

    Type type = UKNOWN_TYPE;

    String column_name;

    /// For DROP COLUMN ... FROM PARTITION
    String partition_name;

    /// For ADD and MODIFY, a new column type.
    DataTypePtr data_type;

    ColumnDefaultKind default_kind{};
    ASTPtr default_expression{};
    String comment;

    /// For ADD - after which column to add a new one. If an empty string, add to the end. To add to the beginning now it is impossible.
    String after_column;

    /// For DROP_COLUMN, MODIFY_COLUMN, COMMENT_COLUMN
    bool if_exists = false;

    /// For ADD_COLUMN
    bool if_not_exists = false;

    /// For MODIFY_ORDER_BY
    ASTPtr order_by;

    /// For ADD INDEX
    ASTPtr index_decl;
    String after_index_name;

    /// For ADD/DROP INDEX
    String index_name;

    /// For MODIFY TTL
    ASTPtr ttl;

    /// indicates that this command should not be applied, for example in case of if_exists=true and column doesn't exist.
    bool ignore = false;

    /// For ADD and MODIFY
    CompressionCodecPtr codec;

    AlterCommand() = default;
    AlterCommand(const Type type_, const String & column_name_, const DataTypePtr & data_type_,
                 const ColumnDefaultKind default_kind_, const ASTPtr & default_expression_,
                 const String & after_column_, const String & comment_,
                 const bool if_exists_, const bool if_not_exists_)
        : type{type_}, column_name{column_name_}, data_type{data_type_}, default_kind{default_kind_},
        default_expression{default_expression_}, comment(comment_), after_column{after_column_},
        if_exists(if_exists_), if_not_exists(if_not_exists_)
    {}

    static std::optional<AlterCommand> parse(const ASTAlterCommand * command);

    void apply(ColumnsDescription & columns_description, IndicesDescription & indices_description,
            ASTPtr & order_by_ast, ASTPtr & primary_key_ast, ASTPtr & ttl_table_ast) const;

    /// Checks that not only metadata touched by that command
    bool isMutable() const;
};

class Context;

class AlterCommands : public std::vector<AlterCommand>
{
public:
    void apply(ColumnsDescription & columns_description, IndicesDescription & indices_description, ASTPtr & order_by_ast,
            ASTPtr & primary_key_ast, ASTPtr & ttl_table_ast) const;

    /// For storages that don't support MODIFY_ORDER_BY.
    void apply(ColumnsDescription & columns_description) const;

    void validate(const IStorage & table, const Context & context);
    bool isMutable() const;
};

}
