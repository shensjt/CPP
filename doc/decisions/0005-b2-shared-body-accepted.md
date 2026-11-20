# ADR-0005：B2 共享 body 采纳

- 状态：Accepted（2026-11-20）
- 日期：2026-11-20
- 关联：[B2 数据](../benchmarks/2026-11-20-b2-copy.md)
- 取代：无

## 背景

基线显示 1MB 文件只有 645 req/s，主要成本包括：

- LRU `Get` 拷贝整个 `CachedFile`，包括 body；
- `HttpResponse::ToString` 再拷贝一次 body。

## 决策

B2 采纳：

- `CachedFile.body` 改为 `shared_ptr<const std::string>`；
- `LruCache::Get` 只拷贝 shared_ptr；
- `HttpResponse` 支持共享 body；
- 大于 64KB 的响应分 header/body 两次发送，避免再次拷贝；
- 小响应仍一次 `ToString`，避免多一次 flush。

## 证据

1MB 文件，4 worker，32 连接，3 次 × 30s：

```text
QPS    566 → 653   (+15.4%)
P50    50.35 → 43.97ms
P99    300.88 → 132.81ms  (-56%)
```

小文件 8 worker：

```text
QPS    15,508 → 17,449
P99    54.22 → 26.33ms
```

小文件 baseline 波动较大，但没有回退。

## 备选

- 继续用 `ToString`：否决，1MB 多一次整块拷贝；
- 所有响应都分两次发送：否决，小文件多一次 flush，首轮实测变慢；
- 只改 LRU 不改响应：收益不足。

## 后果

- `HttpResponse` 增加 `SetBodyShared` / `HeaderToString`；
- `StaticFileHandler` 需要保证共享 body 生命周期；
- 64KB 阈值是当前实测结果，后续可按数据调整；
- B3 sendfile 继续针对大文件的内核拷贝。
