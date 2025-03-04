/*
 * Copyright (2022) Bytedance Ltd. and/or its affiliates
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <TSO/TSOImpl.h>
#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <Protos/RPCHelpers.h>
#include <common/logger_useful.h>

#include <atomic>
#include <memory>
#include <thread>

namespace DB
{

namespace ErrorCodes
{
    extern const int TSO_TIMESTAMP_NOT_FOUND_ERROR;
    extern const int TSO_TIMESTAMPS_SIZE_TOO_LARGE;
    extern const int TSO_INTERNAL_ERROR;
}

namespace TSO
{

TSOImpl::TSOImpl() = default;
TSOImpl::~TSOImpl() = default;

/** Here we make the setting operation of TSO value atomically.
  * Because if the physical time and logical time are set separately,
  * the timestamp between the setting of physical time and logical time will be unexpected.
  * For example:
  * The current TSO is P1_L1, and if a client asks for a TSO right after physical time setting is just finished but logical part has not,
  * then the new TSO will be P2_L1. After the logical setting operation finishes, the next TSO will be P2_0 because the logical part is refreshed.
  * So this latest TSO P2_0 will be smaller than the older TSO P2_L1. This is not as expected.
  */
void TSOImpl::setPhysicalTime(UInt64 physical_time)
{
    UInt64 new_ts = physical_logical_to_ts(physical_time, 0);
    ts.store(new_ts, std::memory_order_release);
}

UInt64 TSOImpl::fetchAddLogical(UInt32 to_add)
{
    UInt64 timestamp = ts.fetch_add(to_add, std::memory_order_acquire);
    UInt32 next_logical = ts_to_logical(timestamp) + to_add;
    checkLogicalClock(next_logical);
    return timestamp;

}

void TSOImpl::GetTimestamp(
    ::google::protobuf::RpcController *,
    const ::DB::TSO::GetTimestampReq * /*request*/,
    ::DB::TSO::GetTimestampResp *response,
    ::google::protobuf::Closure *done)
{
    brpc::ClosureGuard done_guard(done);

    try
    {
        if (!is_leader.load(std::memory_order_acquire))
        {
            response->set_is_leader(false);
            return;
        }

        UInt64 cur_ts = fetchAddLogical(1);
        if (ts_to_physical(cur_ts) == 0)
            throw Exception("Timestamp not found.", ErrorCodes::TSO_TIMESTAMP_NOT_FOUND_ERROR);

        response->set_timestamp(cur_ts);
        response->set_is_leader(true);
    }
    catch (...)
    {
        tryLogCurrentException(log, __PRETTY_FUNCTION__);
        RPCHelpers::handleException(response->mutable_exception());
    }
}

void TSOImpl::GetTimestamps(::google::protobuf::RpcController *,
                            const ::DB::TSO::GetTimestampsReq * request,
                            ::DB::TSO::GetTimestampsResp *response,
                            ::google::protobuf::Closure *done)
{
    brpc::ClosureGuard done_guard(done);

    try
    {
        if (!is_leader.load(std::memory_order_acquire))
        {
            response->set_is_leader(false);
            return;
        }

        /// avoid requesting zero timestamp
        UInt32 size = request->size() ? request->size() : 1;
        if (size > MAX_LOGICAL / 8)
            throw Exception("Size of requested timestamps is too large.", ErrorCodes::TSO_TIMESTAMPS_SIZE_TOO_LARGE);

        UInt64 cur_ts = fetchAddLogical(size);
        UInt64 physical = ts_to_physical(cur_ts);
        if (physical == 0)
            throw Exception("Timestamp not found.", ErrorCodes::TSO_TIMESTAMP_NOT_FOUND_ERROR);

        UInt32 logical = ts_to_logical(cur_ts) + size - 1;
        UInt64 max_ts = physical_logical_to_ts(physical, logical);
        response->set_max_timestamp(max_ts);
        response->set_is_leader(true);
    }
    catch (...)
    {
        tryLogCurrentException(log, __PRETTY_FUNCTION__);
        RPCHelpers::handleException(response->mutable_exception());
    }
}

void TSOImpl::checkLogicalClock(UInt32 logical_value)
{
    if (likely(logical_value < MAX_LOGICAL)) return;

    bool old_value = false;
    if (logical_clock_checking.compare_exchange_strong(old_value, true, std::memory_order_seq_cst, std::memory_order_relaxed))
    {
        /// Launch another thread to check whether updateTSO() is dead
        ThreadFromGlobalPool([this] {
            try
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(TSO_UPDATE_INTERVAL));
                /// Check the leader result in case the node yielded the leadership during sleeping
                TSOClock cur_ts = getClock();
                if (exitLeaderElection && is_leader.load(std::memory_order_acquire) && cur_ts.logical >= MAX_LOGICAL)
                {
                    // Failback to leader election if an overflow issue happens even after sleep_for(TSO_UPDATE_INTERVAL).
                    setIsLeader(false);
                    exitLeaderElection(); // yield leadership as updateTSO thread stopped functioning
                    LOG_DEBUG(log, "Resign leader. TSO logical clock overflow. Physical: {} | Logical: {}", cur_ts.physical, cur_ts.logical);
                }
                logical_clock_checking.store(false, std::memory_order_relaxed);
            }
            catch (...)
            {
                logical_clock_checking.store(false, std::memory_order_relaxed);
                tryLogCurrentException(log);
            }
        }).detach();
    }

    TSOClock cur_ts = getClock();
    throw Exception("GetTimestamp: TSO logical clock overflow. Physical: " + std::to_string(cur_ts.physical) + " | Logical: " + std::to_string(cur_ts.logical), ErrorCodes::TSO_INTERNAL_ERROR);
}

}

}
