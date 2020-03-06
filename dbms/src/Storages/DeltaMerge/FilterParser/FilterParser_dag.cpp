#include <Storages/DeltaMerge/FilterParser/FilterParser.h>

#include <cassert>

#include <common/logger_useful.h>

#include <Flash/Coprocessor/DAGCodec.h>
#include <Flash/Coprocessor/DAGQueryInfo.h>
#include <Flash/Coprocessor/DAGUtils.h>
#include <Poco/Logger.h>


namespace DB
{

namespace ErrorCodes
{
extern const int COP_BAD_DAG_REQUEST;
} // namespace ErrorCodes

namespace DM
{

namespace cop
{

ColumnID getColumnIDForColumnExpr(const tipb::Expr & expr, const ColumnDefines & columns_to_read)
{
    assert(isColumnExpr(expr));
    auto column_index = decodeDAGInt64(expr.val());
    if (column_index < 0 || column_index >= static_cast<Int64>(columns_to_read.size()))
    {
        throw Exception("Column index out of bound: " + DB::toString(column_index) + ", should in [0,"
                            + DB::toString(columns_to_read.size()) + ")",
                        ErrorCodes::COP_BAD_DAG_REQUEST);
    }
    return columns_to_read[column_index].id;
}

inline RSOperatorPtr parseTiCompareExpr( //
    const tipb::Expr &                          expr,
    const FilterParser::RSFilterType            filter_type,
    const ColumnDefines &                       columns_to_read,
    const FilterParser::AttrCreatorByColumnID & creator,
    Poco::Logger * /* log */)
{
    if (unlikely(expr.children_size() != 2))
        return createUnsupported(expr.DebugString(),
                                 tipb::ScalarFuncSig_Name(expr.sig()) + " with " + DB::toString(expr.children_size())
                                     + " children is not supported",
                                 false);

    /// Only support `column` `op` `constant` now.

    Attr             attr;
    Field            value;
    UInt32           state             = 0x0;
    constexpr UInt32 state_has_column  = 0x1;
    constexpr UInt32 state_has_literal = 0x2;
    constexpr UInt32 state_finish      = state_has_column | state_has_literal;
    for (const auto & child : expr.children())
    {
        if (isColumnExpr(child))
        {
            state |= state_has_column;
            ColumnID id = getColumnIDForColumnExpr(child, columns_to_read);
            attr        = creator(id);
        }
        else if (isLiteralExpr(child))
        {
            state |= state_has_literal;
            value = decodeLiteral(child);
        }
    }

    if (unlikely(state != state_finish))
        return createUnsupported(
            expr.DebugString(), tipb::ScalarFuncSig_Name(expr.sig()) + " with state " + DB::toString(state) + " is not supported", false);
    else
    {
        // TODO: null_direction
        RSOperatorPtr op;
        switch (filter_type)
        {
        case FilterParser::RSFilterType::Equal:
            op = createEqual(attr, value);
            break;
        case FilterParser::RSFilterType::NotEqual:
            op = createNotEqual(attr, value);
            break;
        case FilterParser::RSFilterType::Greater:
            op = createGreater(attr, value, -1);
            break;
        case FilterParser::RSFilterType::GreaterEqual:
            op = createGreaterEqual(attr, value, -1);
            break;
        case FilterParser::RSFilterType::Less:
            op = createLess(attr, value, -1);
            break;
        case FilterParser::RSFilterType::LessEuqal:
            op = createLessEqual(attr, value, -1);
            break;
        default:
            op = createUnsupported(expr.DebugString(), "Unknown compare type: " + tipb::ExprType_Name(expr.tp()), false);
            break;
        }
        return op;
    }
}

RSOperatorPtr parseTiExpr(const tipb::Expr &                          expr,
                          const ColumnDefines &                       columns_to_read,
                          const FilterParser::AttrCreatorByColumnID & creator,
                          Poco::Logger *                              log)
{
    assert(isFunctionExpr(expr));

    RSOperatorPtr op = EMPTY_FILTER;
    if (unlikely(isAggFunctionExpr(expr)))
    {
        op = createUnsupported(expr.DebugString(), "agg function: " + tipb::ExprType_Name(expr.tp()), false);
        return op;
    }


    if (auto iter = FilterParser::scalar_func_rs_filter_map.find(expr.sig()); iter != FilterParser::scalar_func_rs_filter_map.end())
    {
        FilterParser::RSFilterType filter_type = iter->second;
        switch (filter_type)
        {
        case FilterParser::RSFilterType::Not:
        {
            if (unlikely(expr.children_size() != 1))
                op = createUnsupported(expr.DebugString(), "logical not with " + DB::toString(expr.children_size()) + " children", false);
            else
            {
                const auto & child = expr.children(0);
                if (likely(isFunctionExpr(child)))
                    op = createNot(parseTiExpr(child, columns_to_read, creator, log));
                else
                    op = createUnsupported(child.DebugString(), "child of logical not is not function", false);
            }
        }
        break;

        case FilterParser::RSFilterType::And:
        case FilterParser::RSFilterType::Or:
        {
            RSOperators children;
            for (Int32 i = 0; i < expr.children_size(); ++i)
            {
                const auto & child = expr.children(i);
                if (likely(isFunctionExpr(child)))
                    children.emplace_back(parseTiExpr(child, columns_to_read, creator, log));
                else
                    children.emplace_back(createUnsupported(child.DebugString(), "child of logical operator is not function", false));
            }
            if (expr.sig() == tipb::ScalarFuncSig::LogicalAnd)
                op = createAnd(children);
            else
                op = createOr(children);
        }
        break;

        case FilterParser::RSFilterType::Equal:
        case FilterParser::RSFilterType::NotEqual:
        case FilterParser::RSFilterType::Greater:
        case FilterParser::RSFilterType::GreaterEqual:
        case FilterParser::RSFilterType::Less:
        case FilterParser::RSFilterType::LessEuqal:
            op = parseTiCompareExpr(expr, filter_type, columns_to_read, creator, log);
            break;

        case FilterParser::RSFilterType::In:
        case FilterParser::RSFilterType::NotIn:
        case FilterParser::RSFilterType::Like:
        case FilterParser::RSFilterType::NotLike:
        case FilterParser::RSFilterType::Unsupported:
            op = createUnsupported(expr.DebugString(), tipb::ScalarFuncSig_Name(expr.sig()) + " is not supported", false);
            break;
        }
    }
    else
    {
        op = createUnsupported(expr.DebugString(), tipb::ScalarFuncSig_Name(expr.sig()) + " is not supported", false);
    }

    return op;
}

} // namespace cop


RSOperatorPtr FilterParser::parseDAGQuery(const DAGQueryInfo &                   dag_info,
                                          const ColumnDefines &                  columns_to_read,
                                          FilterParser::AttrCreatorByColumnID && creator,
                                          Poco::Logger *                         log)
{
    RSOperatorPtr op = EMPTY_FILTER;
    if (dag_info.filters.empty())
        return op;

    if (dag_info.filters.size() == 1)
        op = cop::parseTiExpr(*dag_info.filters[0], columns_to_read, creator, log);
    else
    {
        /// By default, multiple conditions with operator "and"
        RSOperators children;
        for (size_t i = 0; i < dag_info.filters.size(); ++i)
        {
            const auto & filter = *dag_info.filters[i];
            if (isFunctionExpr(filter))
                children.emplace_back(cop::parseTiExpr(filter, columns_to_read, creator, log));
            else
                children.emplace_back(createUnsupported(filter.DebugString(), "child of logical and is not function", false));
        }
        op = createAnd(children);
    }
    return op;
}

std::unordered_map<tipb::ScalarFuncSig, FilterParser::RSFilterType> FilterParser::scalar_func_rs_filter_map{
    /*
    {tipb::ScalarFuncSig::CastIntAsInt, "cast"},
    {tipb::ScalarFuncSig::CastIntAsReal, "cast"},
    {tipb::ScalarFuncSig::CastIntAsString, "cast"},
    {tipb::ScalarFuncSig::CastIntAsDecimal, "cast"},
    {tipb::ScalarFuncSig::CastIntAsTime, "cast"},
    {tipb::ScalarFuncSig::CastIntAsDuration, "cast"},
    {tipb::ScalarFuncSig::CastIntAsJson, "cast"},

    {tipb::ScalarFuncSig::CastRealAsInt, "cast"},
    {tipb::ScalarFuncSig::CastRealAsReal, "cast"},
    {tipb::ScalarFuncSig::CastRealAsString, "cast"},
    {tipb::ScalarFuncSig::CastRealAsDecimal, "cast"},
    {tipb::ScalarFuncSig::CastRealAsTime, "cast"},
    {tipb::ScalarFuncSig::CastRealAsDuration, "cast"},
    {tipb::ScalarFuncSig::CastRealAsJson, "cast"},

    {tipb::ScalarFuncSig::CastDecimalAsInt, "cast"},
    {tipb::ScalarFuncSig::CastDecimalAsReal, "cast"},
    {tipb::ScalarFuncSig::CastDecimalAsString, "cast"},
    {tipb::ScalarFuncSig::CastDecimalAsDecimal, "cast"},
    {tipb::ScalarFuncSig::CastDecimalAsTime, "cast"},
    {tipb::ScalarFuncSig::CastDecimalAsDuration, "cast"},
    {tipb::ScalarFuncSig::CastDecimalAsJson, "cast"},

    {tipb::ScalarFuncSig::CastStringAsInt, "cast"},
    {tipb::ScalarFuncSig::CastStringAsReal, "cast"},
    {tipb::ScalarFuncSig::CastStringAsString, "cast"},
    {tipb::ScalarFuncSig::CastStringAsDecimal, "cast"},
    {tipb::ScalarFuncSig::CastStringAsTime, "cast"},
    {tipb::ScalarFuncSig::CastStringAsDuration, "cast"},
    {tipb::ScalarFuncSig::CastStringAsJson, "cast"},

    {tipb::ScalarFuncSig::CastTimeAsInt, "cast"},
    {tipb::ScalarFuncSig::CastTimeAsReal, "cast"},
    {tipb::ScalarFuncSig::CastTimeAsString, "cast"},
    {tipb::ScalarFuncSig::CastTimeAsDecimal, "cast"},
    {tipb::ScalarFuncSig::CastTimeAsTime, "cast"},
    {tipb::ScalarFuncSig::CastTimeAsDuration, "cast"},
    {tipb::ScalarFuncSig::CastTimeAsJson, "cast"},

    {tipb::ScalarFuncSig::CastDurationAsInt, "cast"},
    {tipb::ScalarFuncSig::CastDurationAsReal, "cast"},
    {tipb::ScalarFuncSig::CastDurationAsString, "cast"},
    {tipb::ScalarFuncSig::CastDurationAsDecimal, "cast"},
    {tipb::ScalarFuncSig::CastDurationAsTime, "cast"},
    {tipb::ScalarFuncSig::CastDurationAsDuration, "cast"},
    {tipb::ScalarFuncSig::CastDurationAsJson, "cast"},

    {tipb::ScalarFuncSig::CastJsonAsInt, "cast"},
    {tipb::ScalarFuncSig::CastJsonAsReal, "cast"},
    {tipb::ScalarFuncSig::CastJsonAsString, "cast"},
    {tipb::ScalarFuncSig::CastJsonAsDecimal, "cast"},
    {tipb::ScalarFuncSig::CastJsonAsTime, "cast"},
    {tipb::ScalarFuncSig::CastJsonAsDuration, "cast"},
    {tipb::ScalarFuncSig::CastJsonAsJson, "cast"},

    {tipb::ScalarFuncSig::CoalesceInt, "coalesce"},
    {tipb::ScalarFuncSig::CoalesceReal, "coalesce"},
    {tipb::ScalarFuncSig::CoalesceString, "coalesce"},
    {tipb::ScalarFuncSig::CoalesceDecimal, "coalesce"},
    {tipb::ScalarFuncSig::CoalesceTime, "coalesce"},
    {tipb::ScalarFuncSig::CoalesceDuration, "coalesce"},
    {tipb::ScalarFuncSig::CoalesceJson, "coalesce"},
    */

    {tipb::ScalarFuncSig::LTInt, FilterParser::RSFilterType::Less},
    {tipb::ScalarFuncSig::LTReal, FilterParser::RSFilterType::Less},
    {tipb::ScalarFuncSig::LTString, FilterParser::RSFilterType::Less},
    {tipb::ScalarFuncSig::LTDecimal, FilterParser::RSFilterType::Less},
    {tipb::ScalarFuncSig::LTTime, FilterParser::RSFilterType::Less},
    {tipb::ScalarFuncSig::LTDuration, FilterParser::RSFilterType::Less},
    {tipb::ScalarFuncSig::LTJson, FilterParser::RSFilterType::Less},

    {tipb::ScalarFuncSig::LEInt, FilterParser::RSFilterType::LessEuqal},
    {tipb::ScalarFuncSig::LEReal, FilterParser::RSFilterType::LessEuqal},
    {tipb::ScalarFuncSig::LEString, FilterParser::RSFilterType::LessEuqal},
    {tipb::ScalarFuncSig::LEDecimal, FilterParser::RSFilterType::LessEuqal},
    {tipb::ScalarFuncSig::LETime, FilterParser::RSFilterType::LessEuqal},
    {tipb::ScalarFuncSig::LEDuration, FilterParser::RSFilterType::LessEuqal},
    {tipb::ScalarFuncSig::LEJson, FilterParser::RSFilterType::LessEuqal},

    {tipb::ScalarFuncSig::GTInt, FilterParser::RSFilterType::Greater},
    {tipb::ScalarFuncSig::GTReal, FilterParser::RSFilterType::Greater},
    {tipb::ScalarFuncSig::GTString, FilterParser::RSFilterType::Greater},
    {tipb::ScalarFuncSig::GTDecimal, FilterParser::RSFilterType::Greater},
    {tipb::ScalarFuncSig::GTTime, FilterParser::RSFilterType::Greater},
    {tipb::ScalarFuncSig::GTDuration, FilterParser::RSFilterType::Greater},
    {tipb::ScalarFuncSig::GTJson, FilterParser::RSFilterType::Greater},

    // {tipb::ScalarFuncSig::GreatestInt, "greatest"},
    // {tipb::ScalarFuncSig::GreatestReal, "greatest"},
    // {tipb::ScalarFuncSig::GreatestString, "greatest"},
    // {tipb::ScalarFuncSig::GreatestDecimal, "greatest"},
    // {tipb::ScalarFuncSig::GreatestTime, "greatest"},

    // {tipb::ScalarFuncSig::LeastInt, "least"},
    // {tipb::ScalarFuncSig::LeastReal, "least"},
    // {tipb::ScalarFuncSig::LeastString, "least"},
    // {tipb::ScalarFuncSig::LeastDecimal, "least"},
    // {tipb::ScalarFuncSig::LeastTime, "least"},

    //{tipb::ScalarFuncSig::IntervalInt, "cast"},
    //{tipb::ScalarFuncSig::IntervalReal, "cast"},

    {tipb::ScalarFuncSig::GEInt, FilterParser::RSFilterType::GreaterEqual},
    {tipb::ScalarFuncSig::GEReal, FilterParser::RSFilterType::GreaterEqual},
    {tipb::ScalarFuncSig::GEString, FilterParser::RSFilterType::GreaterEqual},
    {tipb::ScalarFuncSig::GEDecimal, FilterParser::RSFilterType::GreaterEqual},
    {tipb::ScalarFuncSig::GETime, FilterParser::RSFilterType::GreaterEqual},
    {tipb::ScalarFuncSig::GEDuration, FilterParser::RSFilterType::GreaterEqual},
    {tipb::ScalarFuncSig::GEJson, FilterParser::RSFilterType::GreaterEqual},

    {tipb::ScalarFuncSig::EQInt, FilterParser::RSFilterType::Equal},
    {tipb::ScalarFuncSig::EQReal, FilterParser::RSFilterType::Equal},
    {tipb::ScalarFuncSig::EQString, FilterParser::RSFilterType::Equal},
    {tipb::ScalarFuncSig::EQDecimal, FilterParser::RSFilterType::Equal},
    {tipb::ScalarFuncSig::EQTime, FilterParser::RSFilterType::Equal},
    {tipb::ScalarFuncSig::EQDuration, FilterParser::RSFilterType::Equal},
    {tipb::ScalarFuncSig::EQJson, FilterParser::RSFilterType::Equal},

    {tipb::ScalarFuncSig::NEInt, FilterParser::RSFilterType::NotEqual},
    {tipb::ScalarFuncSig::NEReal, FilterParser::RSFilterType::NotEqual},
    {tipb::ScalarFuncSig::NEString, FilterParser::RSFilterType::NotEqual},
    {tipb::ScalarFuncSig::NEDecimal, FilterParser::RSFilterType::NotEqual},
    {tipb::ScalarFuncSig::NETime, FilterParser::RSFilterType::NotEqual},
    {tipb::ScalarFuncSig::NEDuration, FilterParser::RSFilterType::NotEqual},
    {tipb::ScalarFuncSig::NEJson, FilterParser::RSFilterType::NotEqual},

    //{tipb::ScalarFuncSig::NullEQInt, "cast"},
    //{tipb::ScalarFuncSig::NullEQReal, "cast"},
    //{tipb::ScalarFuncSig::NullEQString, "cast"},
    //{tipb::ScalarFuncSig::NullEQDecimal, "cast"},
    //{tipb::ScalarFuncSig::NullEQTime, "cast"},
    //{tipb::ScalarFuncSig::NullEQDuration, "cast"},
    //{tipb::ScalarFuncSig::NullEQJson, "cast"},

    // {tipb::ScalarFuncSig::PlusReal, "plus"},
    // {tipb::ScalarFuncSig::PlusDecimal, "plus"},
    // {tipb::ScalarFuncSig::PlusInt, "plus"},

    // {tipb::ScalarFuncSig::MinusReal, "minus"},
    // {tipb::ScalarFuncSig::MinusDecimal, "minus"},
    // {tipb::ScalarFuncSig::MinusInt, "minus"},

    // {tipb::ScalarFuncSig::MultiplyReal, "multiply"},
    // {tipb::ScalarFuncSig::MultiplyDecimal, "multiply"},
    // {tipb::ScalarFuncSig::MultiplyInt, "multiply"},

    // {tipb::ScalarFuncSig::DivideReal, "divide"},
    // {tipb::ScalarFuncSig::DivideDecimal, "divide"},
    // {tipb::ScalarFuncSig::IntDivideInt, "intDiv"},
    // {tipb::ScalarFuncSig::IntDivideDecimal, "divide"},

    // {tipb::ScalarFuncSig::ModReal, "modulo"},
    // {tipb::ScalarFuncSig::ModDecimal, "modulo"},
    // {tipb::ScalarFuncSig::ModInt, "modulo"},

    // {tipb::ScalarFuncSig::MultiplyIntUnsigned, "multiply"},

    // {tipb::ScalarFuncSig::AbsInt, "abs"},
    // {tipb::ScalarFuncSig::AbsUInt, "abs"},
    // {tipb::ScalarFuncSig::AbsReal, "abs"},
    // {tipb::ScalarFuncSig::AbsDecimal, "abs"},

    // {tipb::ScalarFuncSig::CeilIntToDec, "ceil"},
    // {tipb::ScalarFuncSig::CeilIntToInt, "ceil"},
    // {tipb::ScalarFuncSig::CeilDecToInt, "ceil"},
    // {tipb::ScalarFuncSig::CeilDecToDec, "ceil"},
    // {tipb::ScalarFuncSig::CeilReal, "ceil"},

    // {tipb::ScalarFuncSig::FloorIntToDec, "floor"},
    // {tipb::ScalarFuncSig::FloorIntToInt, "floor"},
    // {tipb::ScalarFuncSig::FloorDecToInt, "floor"},
    // {tipb::ScalarFuncSig::FloorDecToDec, "floor"},
    // {tipb::ScalarFuncSig::FloorReal, "floor"},

    //{tipb::ScalarFuncSig::RoundReal, "round"},
    //{tipb::ScalarFuncSig::RoundInt, "round"},
    //{tipb::ScalarFuncSig::RoundDec, "round"},
    //{tipb::ScalarFuncSig::RoundWithFracReal, "cast"},
    //{tipb::ScalarFuncSig::RoundWithFracInt, "cast"},
    //{tipb::ScalarFuncSig::RoundWithFracDec, "cast"},

    //{tipb::ScalarFuncSig::Log1Arg, "log"},
    //{tipb::ScalarFuncSig::Log2Args, "cast"},
    //{tipb::ScalarFuncSig::Log2, "log2"},
    //{tipb::ScalarFuncSig::Log10, "log10"},

    //{tipb::ScalarFuncSig::Rand, "rand"},
    //{tipb::ScalarFuncSig::RandWithSeed, "cast"},

    //{tipb::ScalarFuncSig::Pow, "pow"},
    //{tipb::ScalarFuncSig::Conv, "cast"},
    //{tipb::ScalarFuncSig::CRC32, "cast"},
    //{tipb::ScalarFuncSig::Sign, "cast"},

    //{tipb::ScalarFuncSig::Sqrt, "sqrt"},
    //{tipb::ScalarFuncSig::Acos, "acos"},
    //{tipb::ScalarFuncSig::Asin, "asin"},
    //{tipb::ScalarFuncSig::Atan1Arg, "atan"},
    //{tipb::ScalarFuncSig::Atan2Args, "cast"},
    //{tipb::ScalarFuncSig::Cos, "cos"},
    //{tipb::ScalarFuncSig::Cot, "cast"},
    //{tipb::ScalarFuncSig::Degrees, "cast"},
    //{tipb::ScalarFuncSig::Exp, "exp"},
    //{tipb::ScalarFuncSig::PI, "cast"},
    //{tipb::ScalarFuncSig::Radians, "cast"},
    // {tipb::ScalarFuncSig::Sin, "sin"},
    // {tipb::ScalarFuncSig::Tan, "tan"},
    // {tipb::ScalarFuncSig::TruncateInt, "trunc"},
    // {tipb::ScalarFuncSig::TruncateReal, "trunc"},
    //{tipb::ScalarFuncSig::TruncateDecimal, "cast"},

    {tipb::ScalarFuncSig::LogicalAnd, FilterParser::RSFilterType::And},
    {tipb::ScalarFuncSig::LogicalOr, FilterParser::RSFilterType::Or},
    // {tipb::ScalarFuncSig::LogicalXor, "xor"},
    {tipb::ScalarFuncSig::UnaryNotDecimal, FilterParser::RSFilterType::Not},
    {tipb::ScalarFuncSig::UnaryNotInt, FilterParser::RSFilterType::Not},
    {tipb::ScalarFuncSig::UnaryNotReal, FilterParser::RSFilterType::Not},

    // {tipb::ScalarFuncSig::UnaryMinusInt, "negate"},
    // {tipb::ScalarFuncSig::UnaryMinusReal, "negate"},
    // {tipb::ScalarFuncSig::UnaryMinusDecimal, "negate"},

    // {tipb::ScalarFuncSig::DecimalIsNull, "isNull"},
    // {tipb::ScalarFuncSig::DurationIsNull, "isNull"},
    // {tipb::ScalarFuncSig::RealIsNull, "isNull"},
    // {tipb::ScalarFuncSig::StringIsNull, "isNull"},
    // {tipb::ScalarFuncSig::TimeIsNull, "isNull"},
    // {tipb::ScalarFuncSig::IntIsNull, "isNull"},
    // {tipb::ScalarFuncSig::JsonIsNull, "isNull"},

    //{tipb::ScalarFuncSig::BitAndSig, "cast"},
    //{tipb::ScalarFuncSig::BitOrSig, "cast"},
    //{tipb::ScalarFuncSig::BitXorSig, "cast"},
    //{tipb::ScalarFuncSig::BitNegSig, "cast"},
    //{tipb::ScalarFuncSig::IntIsTrue, "cast"},
    //{tipb::ScalarFuncSig::RealIsTrue, "cast"},
    //{tipb::ScalarFuncSig::DecimalIsTrue, "cast"},
    //{tipb::ScalarFuncSig::IntIsFalse, "cast"},
    //{tipb::ScalarFuncSig::RealIsFalse, "cast"},
    //{tipb::ScalarFuncSig::DecimalIsFalse, "cast"},

    //{tipb::ScalarFuncSig::LeftShift, "cast"},
    //{tipb::ScalarFuncSig::RightShift, "cast"},

    //{tipb::ScalarFuncSig::BitCount, "cast"},
    //{tipb::ScalarFuncSig::GetParamString, "cast"},
    //{tipb::ScalarFuncSig::GetVar, "cast"},
    //{tipb::ScalarFuncSig::RowSig, "cast"},
    //{tipb::ScalarFuncSig::SetVar, "cast"},
    //{tipb::ScalarFuncSig::ValuesDecimal, "cast"},
    //{tipb::ScalarFuncSig::ValuesDuration, "cast"},
    //{tipb::ScalarFuncSig::ValuesInt, "cast"},
    //{tipb::ScalarFuncSig::ValuesJSON, "cast"},
    //{tipb::ScalarFuncSig::ValuesReal, "cast"},
    //{tipb::ScalarFuncSig::ValuesString, "cast"},
    //{tipb::ScalarFuncSig::ValuesTime, "cast"},

    // {tipb::ScalarFuncSig::InInt, "in"},
    // {tipb::ScalarFuncSig::InReal, "in"},
    // {tipb::ScalarFuncSig::InString, "in"},
    // {tipb::ScalarFuncSig::InDecimal, "in"},
    // {tipb::ScalarFuncSig::InTime, "in"},
    // {tipb::ScalarFuncSig::InDuration, "in"},
    // {tipb::ScalarFuncSig::InJson, "in"},

    // {tipb::ScalarFuncSig::IfNullInt, "ifNull"},
    // {tipb::ScalarFuncSig::IfNullReal, "ifNull"},
    // {tipb::ScalarFuncSig::IfNullString, "ifNull"},
    // {tipb::ScalarFuncSig::IfNullDecimal, "ifNull"},
    // {tipb::ScalarFuncSig::IfNullTime, "ifNull"},
    // {tipb::ScalarFuncSig::IfNullDuration, "ifNull"},
    // {tipb::ScalarFuncSig::IfNullJson, "ifNull"},

    // {tipb::ScalarFuncSig::IfInt, "if"},
    // {tipb::ScalarFuncSig::IfReal, "if"},
    // {tipb::ScalarFuncSig::IfString, "if"},
    // {tipb::ScalarFuncSig::IfDecimal, "if"},
    // {tipb::ScalarFuncSig::IfTime, "if"},
    // {tipb::ScalarFuncSig::IfDuration, "if"},
    // {tipb::ScalarFuncSig::IfJson, "if"},

    //todo need further check for caseWithExpression and multiIf
    //{tipb::ScalarFuncSig::CaseWhenInt, "caseWithExpression"},
    //{tipb::ScalarFuncSig::CaseWhenReal, "caseWithExpression"},
    //{tipb::ScalarFuncSig::CaseWhenString, "caseWithExpression"},
    //{tipb::ScalarFuncSig::CaseWhenDecimal, "caseWithExpression"},
    //{tipb::ScalarFuncSig::CaseWhenTime, "caseWithExpression"},
    //{tipb::ScalarFuncSig::CaseWhenDuration, "caseWithExpression"},
    //{tipb::ScalarFuncSig::CaseWhenJson, "caseWithExpression"},

    //{tipb::ScalarFuncSig::AesDecrypt, "cast"},
    //{tipb::ScalarFuncSig::AesEncrypt, "cast"},
    //{tipb::ScalarFuncSig::Compress, "cast"},
    //{tipb::ScalarFuncSig::MD5, "cast"},
    //{tipb::ScalarFuncSig::Password, "cast"},
    //{tipb::ScalarFuncSig::RandomBytes, "cast"},
    //{tipb::ScalarFuncSig::SHA1, "cast"},
    //{tipb::ScalarFuncSig::SHA2, "cast"},
    //{tipb::ScalarFuncSig::Uncompress, "cast"},
    //{tipb::ScalarFuncSig::UncompressedLength, "cast"},

    //{tipb::ScalarFuncSig::Database, "cast"},
    //{tipb::ScalarFuncSig::FoundRows, "cast"},
    //{tipb::ScalarFuncSig::CurrentUser, "cast"},
    //{tipb::ScalarFuncSig::User, "cast"},
    //{tipb::ScalarFuncSig::ConnectionID, "cast"},
    //{tipb::ScalarFuncSig::LastInsertID, "cast"},
    //{tipb::ScalarFuncSig::LastInsertIDWithID, "cast"},
    //{tipb::ScalarFuncSig::Version, "cast"},
    //{tipb::ScalarFuncSig::TiDBVersion, "cast"},
    //{tipb::ScalarFuncSig::RowCount, "cast"},

    //{tipb::ScalarFuncSig::Sleep, "cast"},
    //{tipb::ScalarFuncSig::Lock, "cast"},
    //{tipb::ScalarFuncSig::ReleaseLock, "cast"},
    //{tipb::ScalarFuncSig::DecimalAnyValue, "cast"},
    //{tipb::ScalarFuncSig::DurationAnyValue, "cast"},
    //{tipb::ScalarFuncSig::IntAnyValue, "cast"},
    //{tipb::ScalarFuncSig::JSONAnyValue, "cast"},
    //{tipb::ScalarFuncSig::RealAnyValue, "cast"},
    //{tipb::ScalarFuncSig::StringAnyValue, "cast"},
    //{tipb::ScalarFuncSig::TimeAnyValue, "cast"},
    //{tipb::ScalarFuncSig::InetAton, "cast"},
    //{tipb::ScalarFuncSig::InetNtoa, "cast"},
    //{tipb::ScalarFuncSig::Inet6Aton, "cast"},
    //{tipb::ScalarFuncSig::Inet6Ntoa, "cast"},
    //{tipb::ScalarFuncSig::IsIPv4, "cast"},
    //{tipb::ScalarFuncSig::IsIPv4Compat, "cast"},
    //{tipb::ScalarFuncSig::IsIPv4Mapped, "cast"},
    //{tipb::ScalarFuncSig::IsIPv6, "cast"},
    //{tipb::ScalarFuncSig::UUID, "cast"},

    // {tipb::ScalarFuncSig::LikeSig, "like3Args"},
    //{tipb::ScalarFuncSig::RegexpBinarySig, "cast"},
    //{tipb::ScalarFuncSig::RegexpSig, "cast"},

    //{tipb::ScalarFuncSig::JsonExtractSig, "cast"},
    //{tipb::ScalarFuncSig::JsonUnquoteSig, "cast"},
    //{tipb::ScalarFuncSig::JsonTypeSig, "cast"},
    //{tipb::ScalarFuncSig::JsonSetSig, "cast"},
    //{tipb::ScalarFuncSig::JsonInsertSig, "cast"},
    //{tipb::ScalarFuncSig::JsonReplaceSig, "cast"},
    //{tipb::ScalarFuncSig::JsonRemoveSig, "cast"},
    //{tipb::ScalarFuncSig::JsonMergeSig, "cast"},
    //{tipb::ScalarFuncSig::JsonObjectSig, "cast"},
    //{tipb::ScalarFuncSig::JsonArraySig, "cast"},
    //{tipb::ScalarFuncSig::JsonValidJsonSig, "cast"},
    //{tipb::ScalarFuncSig::JsonContainsSig, "cast"},
    //{tipb::ScalarFuncSig::JsonArrayAppendSig, "cast"},
    //{tipb::ScalarFuncSig::JsonArrayInsertSig, "cast"},
    //{tipb::ScalarFuncSig::JsonMergePatchSig, "cast"},
    //{tipb::ScalarFuncSig::JsonMergePreserveSig, "cast"},
    //{tipb::ScalarFuncSig::JsonContainsPathSig, "cast"},
    //{tipb::ScalarFuncSig::JsonPrettySig, "cast"},
    //{tipb::ScalarFuncSig::JsonQuoteSig, "cast"},
    //{tipb::ScalarFuncSig::JsonSearchSig, "cast"},
    //{tipb::ScalarFuncSig::JsonStorageSizeSig, "cast"},
    //{tipb::ScalarFuncSig::JsonDepthSig, "cast"},
    //{tipb::ScalarFuncSig::JsonKeysSig, "cast"},
    //{tipb::ScalarFuncSig::JsonLengthSig, "cast"},
    //{tipb::ScalarFuncSig::JsonKeys2ArgsSig, "cast"},
    //{tipb::ScalarFuncSig::JsonValidStringSig, "cast"},

    //{tipb::ScalarFuncSig::DateFormatSig, "cast"},
    //{tipb::ScalarFuncSig::DateLiteral, "cast"},
    //{tipb::ScalarFuncSig::DateDiff, "cast"},
    //{tipb::ScalarFuncSig::NullTimeDiff, "cast"},
    //{tipb::ScalarFuncSig::TimeStringTimeDiff, "cast"},
    //{tipb::ScalarFuncSig::DurationDurationTimeDiff, "cast"},
    //{tipb::ScalarFuncSig::DurationDurationTimeDiff, "cast"},
    //{tipb::ScalarFuncSig::StringTimeTimeDiff, "cast"},
    //{tipb::ScalarFuncSig::StringDurationTimeDiff, "cast"},
    //{tipb::ScalarFuncSig::StringStringTimeDiff, "cast"},
    //{tipb::ScalarFuncSig::TimeTimeTimeDiff, "cast"},

    //{tipb::ScalarFuncSig::Date, "cast"},
    //{tipb::ScalarFuncSig::Hour, "cast"},
    //{tipb::ScalarFuncSig::Minute, "cast"},
    //{tipb::ScalarFuncSig::Second, "cast"},
    //{tipb::ScalarFuncSig::MicroSecond, "cast"},
    //{tipb::ScalarFuncSig::Month, "cast"},
    //{tipb::ScalarFuncSig::MonthName, "cast"},

    //{tipb::ScalarFuncSig::NowWithArg, "cast"},
    //{tipb::ScalarFuncSig::NowWithoutArg, "cast"},

    //{tipb::ScalarFuncSig::DayName, "cast"},
    //{tipb::ScalarFuncSig::DayOfMonth, "cast"},
    //{tipb::ScalarFuncSig::DayOfWeek, "cast"},
    //{tipb::ScalarFuncSig::DayOfYear, "cast"},

    //{tipb::ScalarFuncSig::WeekWithMode, "cast"},
    //{tipb::ScalarFuncSig::WeekWithoutMode, "cast"},
    //{tipb::ScalarFuncSig::WeekDay, "cast"},
    //{tipb::ScalarFuncSig::WeekOfYear, "cast"},

    //{tipb::ScalarFuncSig::Year, "cast"},
    //{tipb::ScalarFuncSig::YearWeekWithMode, "cast"},
    //{tipb::ScalarFuncSig::YearWeekWithoutMode, "cast"},

    //{tipb::ScalarFuncSig::GetFormat, "cast"},
    //{tipb::ScalarFuncSig::SysDateWithFsp, "cast"},
    //{tipb::ScalarFuncSig::SysDateWithoutFsp, "cast"},
    //{tipb::ScalarFuncSig::CurrentDate, "cast"},
    //{tipb::ScalarFuncSig::CurrentTime0Arg, "cast"},
    //{tipb::ScalarFuncSig::CurrentTime1Arg, "cast"},

    //{tipb::ScalarFuncSig::Time, "cast"},
    //{tipb::ScalarFuncSig::TimeLiteral, "cast"},
    //{tipb::ScalarFuncSig::UTCDate, "cast"},
    //{tipb::ScalarFuncSig::UTCTimestampWithArg, "cast"},
    //{tipb::ScalarFuncSig::UTCTimestampWithoutArg, "cast"},

    //{tipb::ScalarFuncSig::AddDatetimeAndDuration, "cast"},
    //{tipb::ScalarFuncSig::AddDatetimeAndString, "cast"},
    //{tipb::ScalarFuncSig::AddTimeDateTimeNull, "cast"},
    //{tipb::ScalarFuncSig::AddStringAndDuration, "cast"},
    //{tipb::ScalarFuncSig::AddStringAndString, "cast"},
    //{tipb::ScalarFuncSig::AddTimeStringNull, "cast"},
    //{tipb::ScalarFuncSig::AddDurationAndDuration, "cast"},
    //{tipb::ScalarFuncSig::AddDurationAndString, "cast"},
    //{tipb::ScalarFuncSig::AddTimeDurationNull, "cast"},
    //{tipb::ScalarFuncSig::AddDateAndDuration, "cast"},
    //{tipb::ScalarFuncSig::AddDateAndString, "cast"},

    //{tipb::ScalarFuncSig::SubDateAndDuration, "cast"},
    //{tipb::ScalarFuncSig::SubDateAndString, "cast"},
    //{tipb::ScalarFuncSig::SubTimeDateTimeNull, "cast"},
    //{tipb::ScalarFuncSig::SubStringAndDuration, "cast"},
    //{tipb::ScalarFuncSig::SubStringAndString, "cast"},
    //{tipb::ScalarFuncSig::SubTimeStringNull, "cast"},
    //{tipb::ScalarFuncSig::SubDurationAndDuration, "cast"},
    //{tipb::ScalarFuncSig::SubDurationAndString, "cast"},
    //{tipb::ScalarFuncSig::SubDateAndDuration, "cast"},
    //{tipb::ScalarFuncSig::SubDateAndString, "cast"},

    //{tipb::ScalarFuncSig::UnixTimestampCurrent, "cast"},
    //{tipb::ScalarFuncSig::UnixTimestampInt, "cast"},
    //{tipb::ScalarFuncSig::UnixTimestampDec, "cast"},

    //{tipb::ScalarFuncSig::ConvertTz, "cast"},
    //{tipb::ScalarFuncSig::MakeDate, "cast"},
    //{tipb::ScalarFuncSig::MakeTime, "cast"},
    //{tipb::ScalarFuncSig::PeriodAdd, "cast"},
    //{tipb::ScalarFuncSig::PeriodDiff, "cast"},
    //{tipb::ScalarFuncSig::Quarter, "cast"},

    //{tipb::ScalarFuncSig::SecToTime, "cast"},
    //{tipb::ScalarFuncSig::TimeToSec, "cast"},
    //{tipb::ScalarFuncSig::TimestampAdd, "cast"},
    //{tipb::ScalarFuncSig::ToDays, "cast"},
    //{tipb::ScalarFuncSig::ToSeconds, "cast"},
    //{tipb::ScalarFuncSig::UTCTimeWithArg, "cast"},
    //{tipb::ScalarFuncSig::UTCTimestampWithoutArg, "cast"},
    //{tipb::ScalarFuncSig::Timestamp1Arg, "cast"},
    //{tipb::ScalarFuncSig::Timestamp2Args, "cast"},
    //{tipb::ScalarFuncSig::TimestampLiteral, "cast"},

    //{tipb::ScalarFuncSig::LastDay, "cast"},
    //{tipb::ScalarFuncSig::StrToDateDate, "cast"},
    //{tipb::ScalarFuncSig::StrToDateDatetime, "cast"},
    //{tipb::ScalarFuncSig::StrToDateDuration, "cast"},
    //{tipb::ScalarFuncSig::FromUnixTime1Arg, "cast"},
    //{tipb::ScalarFuncSig::FromUnixTime2Arg, "cast"},
    //{tipb::ScalarFuncSig::ExtractDatetime, "cast"},
    //{tipb::ScalarFuncSig::ExtractDuration, "cast"},

    //{tipb::ScalarFuncSig::AddDateStringString, "cast"},
    //{tipb::ScalarFuncSig::AddDateStringInt, "cast"},
    //{tipb::ScalarFuncSig::AddDateStringDecimal, "cast"},
    //{tipb::ScalarFuncSig::AddDateIntString, "cast"},
    //{tipb::ScalarFuncSig::AddDateIntInt, "cast"},
    //{tipb::ScalarFuncSig::AddDateDatetimeString, "cast"},
    //{tipb::ScalarFuncSig::AddDateDatetimeInt, "cast"},

    //{tipb::ScalarFuncSig::SubDateStringString, "cast"},
    //{tipb::ScalarFuncSig::SubDateStringInt, "cast"},
    //{tipb::ScalarFuncSig::SubDateStringDecimal, "cast"},
    //{tipb::ScalarFuncSig::SubDateIntString, "cast"},
    //{tipb::ScalarFuncSig::SubDateIntInt, "cast"},
    //{tipb::ScalarFuncSig::SubDateDatetimeString, "cast"},
    //{tipb::ScalarFuncSig::SubDateDatetimeInt, "cast"},

    //{tipb::ScalarFuncSig::FromDays, "cast"},
    //{tipb::ScalarFuncSig::TimeFormat, "cast"},
    //{tipb::ScalarFuncSig::TimestampDiff, "cast"},

    //{tipb::ScalarFuncSig::BitLength, "cast"},
    //{tipb::ScalarFuncSig::Bin, "cast"},
    //{tipb::ScalarFuncSig::ASCII, "cast"},
    //{tipb::ScalarFuncSig::Char, "cast"},
    // {tipb::ScalarFuncSig::CharLength, "lengthUTF8"},
    //{tipb::ScalarFuncSig::Concat, "cast"},
    //{tipb::ScalarFuncSig::ConcatWS, "cast"},
    //{tipb::ScalarFuncSig::Convert, "cast"},
    //{tipb::ScalarFuncSig::Elt, "cast"},
    //{tipb::ScalarFuncSig::ExportSet3Arg, "cast"},
    //{tipb::ScalarFuncSig::ExportSet4Arg, "cast"},
    //{tipb::ScalarFuncSig::ExportSet5Arg, "cast"},
    //{tipb::ScalarFuncSig::FieldInt, "cast"},
    //{tipb::ScalarFuncSig::FieldReal, "cast"},
    //{tipb::ScalarFuncSig::FieldString, "cast"},

    //{tipb::ScalarFuncSig::FindInSet, "cast"},
    //{tipb::ScalarFuncSig::Format, "cast"},
    //{tipb::ScalarFuncSig::FormatWithLocale, "cast"},
    //{tipb::ScalarFuncSig::FromBase64, "cast"},
    //{tipb::ScalarFuncSig::HexIntArg, "cast"},
    //{tipb::ScalarFuncSig::HexStrArg, "cast"},
    //{tipb::ScalarFuncSig::Insert, "cast"},
    //{tipb::ScalarFuncSig::InsertBinary, "cast"},
    //{tipb::ScalarFuncSig::Instr, "cast"},
    //{tipb::ScalarFuncSig::InstrBinary, "cast"},

    // {tipb::ScalarFuncSig::LTrim, "ltrim"},
    //{tipb::ScalarFuncSig::Left, "cast"},
    //{tipb::ScalarFuncSig::LeftBinary, "cast"},
    // {tipb::ScalarFuncSig::Length, "length"},
    //{tipb::ScalarFuncSig::Locate2Args, "cast"},
    //{tipb::ScalarFuncSig::Locate3Args, "cast"},
    //{tipb::ScalarFuncSig::LocateBinary2Args, "cast"},
    //{tipb::ScalarFuncSig::LocateBinary3Args, "cast"},

    // {tipb::ScalarFuncSig::Lower, "lower"},
    //{tipb::ScalarFuncSig::Lpad, "cast"},
    //{tipb::ScalarFuncSig::LpadBinary, "cast"},
    //{tipb::ScalarFuncSig::MakeSet, "cast"},
    //{tipb::ScalarFuncSig::OctInt, "cast"},
    //{tipb::ScalarFuncSig::OctString, "cast"},
    //{tipb::ScalarFuncSig::Ord, "cast"},
    //{tipb::ScalarFuncSig::Quote, "cast"},
    // {tipb::ScalarFuncSig::RTrim, "rtrim"},
    //{tipb::ScalarFuncSig::Repeat, "cast"},
    //{tipb::ScalarFuncSig::Replace, "cast"},
    //{tipb::ScalarFuncSig::Reverse, "cast"},
    //{tipb::ScalarFuncSig::ReverseBinary, "cast"},
    //{tipb::ScalarFuncSig::Right, "cast"},
    //{tipb::ScalarFuncSig::RightBinary, "cast"},
    //{tipb::ScalarFuncSig::Rpad, "cast"},
    //{tipb::ScalarFuncSig::RpadBinary, "cast"},
    //{tipb::ScalarFuncSig::Space, "cast"},
    //{tipb::ScalarFuncSig::Strcmp, "cast"},
    //{tipb::ScalarFuncSig::Substring2Args, "cast"},
    //{tipb::ScalarFuncSig::Substring3Args, "cast"},
    //{tipb::ScalarFuncSig::SubstringBinary2Args, "cast"},
    //{tipb::ScalarFuncSig::SubstringBinary3Args, "cast"},
    //{tipb::ScalarFuncSig::SubstringIndex, "cast"},

    //{tipb::ScalarFuncSig::ToBase64, "cast"},
    //{tipb::ScalarFuncSig::Trim1Arg, "cast"},
    //{tipb::ScalarFuncSig::Trim2Args, "cast"},
    //{tipb::ScalarFuncSig::Trim3Args, "cast"},
    //{tipb::ScalarFuncSig::UnHex, "cast"},
    // {tipb::ScalarFuncSig::Upper, "upper"},
};

} // namespace DM

} // namespace DB