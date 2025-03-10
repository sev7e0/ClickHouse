#include <Columns/ColumnConst.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/getLeastSupertype.h>
#include <Functions/FunctionFactory.h>
#include <Functions/IFunction.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/castColumn.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

// Implements function, giving value for column within range of given
// Example:
// | c1 |
// | 10 |
// | 20 |
// SELECT c1, neighbor(c1, 1) as c2:
// | c1 | c2 |
// | 10 | 20 |
// | 20 | 0  |
class FunctionNeighbor : public IFunction
{
public:
    static constexpr auto name = "neighbor";
    static FunctionPtr create(const Context & context) { return std::make_shared<FunctionNeighbor>(context); }

    FunctionNeighbor(const Context & context_) : context(context_) {}

    /// Get the name of the function.
    String getName() const override { return name; }

    size_t getNumberOfArguments() const override { return 0; }

    bool isVariadic() const override { return true; }

    bool isDeterministic() const override { return false; }

    bool isDeterministicInScopeOfQuery() const override { return false; }

    bool useDefaultImplementationForNulls() const override { return false; }

    bool useDefaultImplementationForConstants() const override { return false; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        size_t number_of_arguments = arguments.size();

        if (number_of_arguments < 2 || number_of_arguments > 3)
            throw Exception(
                "Number of arguments for function " + getName() + " doesn't match: passed " + toString(number_of_arguments)
                    + ", should be from 2 to 3",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        // second argument must be an integer
        if (!isInteger(arguments[1]))
            throw Exception(
                "Illegal type " + arguments[1]->getName() + " of second argument of function " + getName() + " - should be an integer",
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        else if (arguments[1]->isNullable())
            throw Exception(
                "Illegal type " + arguments[1]->getName() + " of second argument of function " + getName() + " - can not be Nullable",
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

        // check that default value column has supertype with first argument
        if (number_of_arguments == 3)
            return getLeastSupertype({arguments[0], arguments[2]});

        return arguments[0];
    }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const DataTypePtr & result_type = block.getByPosition(result).type;

        const ColumnWithTypeAndName & source_elem = block.getByPosition(arguments[0]);
        const ColumnWithTypeAndName & offset_elem = block.getByPosition(arguments[1]);
        bool has_defaults = arguments.size() == 3;

        ColumnPtr source_column_casted = castColumn(source_elem, result_type, context);
        ColumnPtr offset_column = offset_elem.column;

        ColumnPtr default_column_casted;
        if (has_defaults)
        {
            const ColumnWithTypeAndName & default_elem = block.getByPosition(arguments[2]);
            default_column_casted = castColumn(default_elem, result_type, context);
        }

        bool source_is_constant = isColumnConst(*source_column_casted);
        bool offset_is_constant = isColumnConst(*offset_column);

        bool default_is_constant = false;
        if (has_defaults)
             default_is_constant = isColumnConst(*default_column_casted);

        if (source_is_constant)
            source_column_casted = assert_cast<const ColumnConst &>(*source_column_casted).getDataColumnPtr();
        if (offset_is_constant)
            offset_column = assert_cast<const ColumnConst &>(*offset_column).getDataColumnPtr();
        if (default_is_constant)
            default_column_casted = assert_cast<const ColumnConst &>(*default_column_casted).getDataColumnPtr();

        auto column = result_type->createColumn();

        for (size_t row = 0; row < input_rows_count; ++row)
        {
            Int64 src_idx = row + offset_column->getInt(offset_is_constant ? 0 : row);

            if (src_idx >= 0 && src_idx < Int64(input_rows_count))
                column->insertFrom(*source_column_casted, source_is_constant ? 0 : src_idx);
            else if (has_defaults)
                column->insertFrom(*default_column_casted, default_is_constant ? 0 : row);
            else
                column->insertDefault();
        }

        block.getByPosition(result).column = std::move(column);
    }

private:
    const Context & context;
};

void registerFunctionNeighbor(FunctionFactory & factory)
{
    factory.registerFunction<FunctionNeighbor>();
}

}
