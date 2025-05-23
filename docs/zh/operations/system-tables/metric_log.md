---
slug: /zh/operations/system-tables/metric_log
machine_translated: true
machine_translated_rev: 5decc73b5dc60054f19087d3690c4eb99446a6c3
---

# 系统。metric_log {#system_tables-metric_log}

包含表中度量值的历史记录 `system.metrics` 和 `system.events`，定期刷新到磁盘。
打开指标历史记录收集 `system.metric_log`,创建 `/etc/clickhouse-server/config.d/metric_log.xml` 具有以下内容:

``` xml
<clickhouse>
    <metric_log>
        <database>system</database>
        <table>metric_log</table>
        <flush_interval_milliseconds>7500</flush_interval_milliseconds>
        <collect_interval_milliseconds>1000</collect_interval_milliseconds>
    </metric_log>
</clickhouse>
```

**示例**

``` sql
SELECT * FROM system.metric_log LIMIT 1 FORMAT Vertical;
```

``` text
Row 1:
──────
event_date:                                                 2020-02-18
event_time:                                                 2020-02-18 07:15:33
milliseconds:                                               554
ProfileEvent_Query:                                         0
ProfileEvent_SelectQuery:                                   0
ProfileEvent_InsertQuery:                                   0
ProfileEvent_FileOpen:                                      0
ProfileEvent_Seek:                                          0
ProfileEvent_ReadBufferFromFileDescriptorRead:              1
ProfileEvent_ReadBufferFromFileDescriptorReadFailed:        0
ProfileEvent_ReadBufferFromFileDescriptorReadBytes:         0
ProfileEvent_WriteBufferFromFileDescriptorWrite:            1
ProfileEvent_WriteBufferFromFileDescriptorWriteFailed:      0
ProfileEvent_WriteBufferFromFileDescriptorWriteBytes:       56
...
CurrentMetric_Query:                                        0
CurrentMetric_Merge:                                        0
CurrentMetric_PartMutation:                                 0
CurrentMetric_ReplicatedFetch:                              0
CurrentMetric_ReplicatedSend:                               0
CurrentMetric_ReplicatedChecks:                             0
...
```

**另请参阅**

-   [系统。asynchronous_metrics](/operations/system-tables/asynchronous_metrics) — Contains periodically calculated metrics.
-   [系统。活动](/operations/system-tables/events) — Contains a number of events that occurred.
-   [系统。指标](/operations/system-tables/metrics) — Contains instantly calculated metrics.
-   [监测](../../operations/monitoring.md) — Base concepts of ClickHouse monitoring.
