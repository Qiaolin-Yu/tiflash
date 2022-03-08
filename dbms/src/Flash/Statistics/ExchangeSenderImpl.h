#pragma once

#include <Flash/Statistics/ConnectionProfileInfo.h>
#include <Flash/Statistics/ExecutorStatistics.h>
#include <tipb/executor.pb.h>

namespace DB
{
struct MPPTunnelDetail : public ConnectionProfileInfo
{
    String tunnel_id;
    Int64 sender_target_task_id;
    String sender_target_host;
    bool is_local;

    MPPTunnelDetail(String tunnel_id_, Int64 sender_target_task_id_, String sender_target_host_, bool is_local_)
        : tunnel_id(std::move(tunnel_id_))
        , sender_target_task_id(sender_target_task_id_)
        , sender_target_host(std::move(sender_target_host_))
        , is_local(is_local_)
    {}

    String toJson() const;
};

struct ExchangeSenderImpl
{
    static constexpr bool has_extra_info = true;

    static constexpr auto type = "ExchangeSender";

    static bool isMatch(const tipb::Executor * executor)
    {
        return executor->has_exchange_sender();
    }
};

using ExchangeSenderStatisticsBase = ExecutorStatistics<ExchangeSenderImpl>;

class ExchangeSenderStatistics : public ExchangeSenderStatisticsBase
{
public:
    ExchangeSenderStatistics(const tipb::Executor * executor, DAGContext & dag_context_);

private:
    UInt16 partition_num;
    tipb::ExchangeType exchange_type;
    std::vector<Int64> sender_target_task_ids;

    std::vector<MPPTunnelDetail> mpp_tunnel_details;

protected:
    void appendExtraJson(FmtBuffer &) const override;
    void collectExtraRuntimeDetail() override;
};
} // namespace DB