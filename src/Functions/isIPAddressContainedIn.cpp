#include <Columns/ColumnConst.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <Common/IPv6ToBinary.h>
#include <Common/formatIPv6.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <variant>
#include <charconv>


#include <Common/logger_useful.h>
namespace DB::ErrorCodes
{
    extern const int CANNOT_PARSE_TEXT;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

namespace
{

class IPAddressVariant
{
public:

    explicit IPAddressVariant(std::string_view address_str)
    {
        UInt32 v4;
        if (DB::parseIPv4whole(address_str.data(), address_str.data() + address_str.size(), reinterpret_cast<unsigned char *>(&v4)))
        {
            addr = v4;
        }
        else
        {
            addr = IPv6AddrType();
            bool success = DB::parseIPv6whole(address_str.data(), address_str.data() + address_str.size(), std::get<IPv6AddrType>(addr).data());
            if (!success)
                throw DB::Exception(DB::ErrorCodes::CANNOT_PARSE_TEXT, "Neither IPv4 nor IPv6 address: '{}'", address_str);
        }
    }

    UInt32 asV4() const
    {
        if (const auto * val = std::get_if<IPv4AddrType>(&addr))
            return *val;
        return 0;
    }

    const uint8_t * asV6() const
    {
        if (const auto * val = std::get_if<IPv6AddrType>(&addr))
            return val->data();
        return nullptr;
    }

private:
    using IPv4AddrType = UInt32;
    using IPv6AddrType = std::array<uint8_t, IPV6_BINARY_LENGTH>;

    std::variant<IPv4AddrType, IPv6AddrType> addr;
};

struct IPAddressCIDR
{
    IPAddressVariant address;
    UInt8 prefix;
};

IPAddressCIDR parseIPWithCIDR(std::string_view cidr_str)
{
    size_t pos_slash = cidr_str.find('/');

    if (pos_slash == 0)
        throw DB::Exception(DB::ErrorCodes::CANNOT_PARSE_TEXT, "Error parsing IP address with prefix: {}", std::string(cidr_str));
    if (pos_slash == std::string_view::npos)
        throw DB::Exception(DB::ErrorCodes::CANNOT_PARSE_TEXT, "The text does not contain '/': {}", std::string(cidr_str));

    std::string_view addr_str = cidr_str.substr(0, pos_slash);
    IPAddressVariant addr(addr_str);

    uint8_t prefix = 0;
    auto prefix_str = cidr_str.substr(pos_slash+1);

    const auto * prefix_str_end = prefix_str.data() + prefix_str.size();
    auto [parse_end, parse_error] = std::from_chars(prefix_str.data(), prefix_str_end, prefix);  /// NOLINT(bugprone-suspicious-stringview-data-usage)
    uint8_t max_prefix = (addr.asV6() ? IPV6_BINARY_LENGTH : IPV4_BINARY_LENGTH) * 8;
    bool has_error = parse_error != std::errc() || parse_end != prefix_str_end || prefix > max_prefix;
    if (has_error)
        throw DB::Exception(DB::ErrorCodes::CANNOT_PARSE_TEXT, "The CIDR has a malformed prefix bits: {}", std::string(cidr_str));

    return {addr, static_cast<UInt8>(prefix)};
}

inline bool isAddressInRange(const IPAddressVariant & address, const IPAddressCIDR & cidr)
{
    if (const auto * cidr_v6 = cidr.address.asV6())
    {
        if (const auto * addr_v6 = address.asV6())
            return DB::matchIPv6Subnet(addr_v6, cidr_v6, cidr.prefix);
    }
    else
    {
        if (!address.asV6())
            return DB::matchIPv4Subnet(address.asV4(), cidr.address.asV4(), cidr.prefix);
    }
    return false;
}

}

namespace DB
{
    class FunctionIsIPAddressContainedIn : public IFunction
    {
    public:
        static constexpr auto name = "isIPAddressInRange";
        String getName() const override { return name; }
        static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionIsIPAddressContainedIn>(); }
        bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

        ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & /* return_type */, size_t input_rows_count) const override
        {
            const IColumn * col_addr = arguments[0].column.get();
            const IColumn * col_cidr = arguments[1].column.get();

            if (const auto * col_addr_const = checkAndGetAnyColumnConst(col_addr))
            {
                if (const auto * col_cidr_const = checkAndGetAnyColumnConst(col_cidr))
                    return executeImpl(*col_addr_const, *col_cidr_const, input_rows_count);
                return executeImpl(*col_addr_const, *col_cidr, input_rows_count);
            }

            if (const auto * col_cidr_const = checkAndGetAnyColumnConst(col_cidr))
                return executeImpl(*col_addr, *col_cidr_const, input_rows_count);
            return executeImpl(*col_addr, *col_cidr, input_rows_count);
        }

        DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
        {
            if (arguments.size() != 2)
                throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                    "Number of arguments for function {} doesn't match: passed {}, should be 2",
                    getName(), arguments.size());

            const DataTypePtr & addr_type = arguments[0];
            const DataTypePtr & prefix_type = arguments[1];

            if (!isString(addr_type) || !isString(prefix_type))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "The arguments of function {} must be String", getName());

            return std::make_shared<DataTypeUInt8>();
        }

        DataTypePtr getReturnTypeForDefaultImplementationForDynamic() const override
        {
            return std::make_shared<DataTypeUInt8>();
        }

        size_t getNumberOfArguments() const override { return 2; }
        bool useDefaultImplementationForNulls() const override { return false; }

    private:
        /// Like checkAndGetColumnConst() but this function doesn't
        /// care about the type of data column.
        static const ColumnConst * checkAndGetAnyColumnConst(const IColumn * column)
        {
            if (!column || !isColumnConst(*column))
                return nullptr;

            return assert_cast<const ColumnConst *>(column);
        }

        /// Both columns are constant.
        static ColumnPtr executeImpl(
            const ColumnConst & col_addr_const,
            const ColumnConst & col_cidr_const,
            size_t input_rows_count)
        {
            const auto & col_addr = col_addr_const.getDataColumn();
            const auto & col_cidr = col_cidr_const.getDataColumn();

            const auto addr = IPAddressVariant(col_addr.getDataAt(0).toView());
            const auto cidr = parseIPWithCIDR(col_cidr.getDataAt(0).toView());

            ColumnUInt8::MutablePtr col_res = ColumnUInt8::create(1);
            ColumnUInt8::Container & vec_res = col_res->getData();

            vec_res[0] = isAddressInRange(addr, cidr) ? 1 : 0;

            return ColumnConst::create(std::move(col_res), input_rows_count);
        }

        /// Address is constant.
        static ColumnPtr executeImpl(const ColumnConst & col_addr_const, const IColumn & col_cidr, size_t input_rows_count)
        {
            const auto & col_addr = col_addr_const.getDataColumn();

            const auto addr = IPAddressVariant(col_addr.getDataAt(0).toView());

            ColumnUInt8::MutablePtr col_res = ColumnUInt8::create(input_rows_count);
            ColumnUInt8::Container & vec_res = col_res->getData();

            for (size_t i = 0; i < input_rows_count; ++i)
            {
                const auto cidr = parseIPWithCIDR(col_cidr.getDataAt(i).toView());
                vec_res[i] = isAddressInRange(addr, cidr) ? 1 : 0;
            }
            return col_res;
        }

        /// CIDR is constant.
        static ColumnPtr executeImpl(const IColumn & col_addr, const ColumnConst & col_cidr_const, size_t input_rows_count)
        {
            const auto & col_cidr = col_cidr_const.getDataColumn();

            const auto cidr = parseIPWithCIDR(col_cidr.getDataAt(0).toView());

            ColumnUInt8::MutablePtr col_res = ColumnUInt8::create(input_rows_count);
            ColumnUInt8::Container & vec_res = col_res->getData();
            for (size_t i = 0; i < input_rows_count; ++i)
            {
                const auto addr = IPAddressVariant(col_addr.getDataAt(i).toView());
                vec_res[i] = isAddressInRange(addr, cidr) ? 1 : 0;
            }
            return col_res;
        }

        /// Neither are constant.
        static ColumnPtr executeImpl(const IColumn & col_addr, const IColumn & col_cidr, size_t input_rows_count)
        {
            ColumnUInt8::MutablePtr col_res = ColumnUInt8::create(input_rows_count);
            ColumnUInt8::Container & vec_res = col_res->getData();

            for (size_t i = 0; i < input_rows_count; ++i)
            {
                const auto addr = IPAddressVariant(col_addr.getDataAt(i).toView());
                const auto cidr = parseIPWithCIDR(col_cidr.getDataAt(i).toView());

                vec_res[i] = isAddressInRange(addr, cidr) ? 1 : 0;
            }

            return col_res;
        }
    };

    REGISTER_FUNCTION(IsIPAddressContainedIn)
    {
        factory.registerFunction<FunctionIsIPAddressContainedIn>();
    }
}
