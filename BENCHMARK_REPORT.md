# 📊 Performance Benchmark Report

**Date:** January 15, 2026  
**Test Dataset:** IMDB Job queries (plans.json)  
**System:** WSL2 Linux

---

## 🏁 Results Summary

| # | Configuration | Query Runtime | Wall Clock | Speedup | Notes |
|---|---|---|---|---|---|
| 1 | **Default (baseline)** | 9,425 ms | 59.4s | **1.0x** | No env vars |
| 2 | JOIN_TELEMETRY=1 | 9,280 ms | 60.1s | 1.016x | Telemetry overhead |
| 3 | AUTO_BUILD_SIDE=0 | 9,733 ms | 56.9s | 0.968x | ❌ Worse |
| 4 | JOIN_TELEMETRY=1 + AUTO_BUILD_SIDE=0 | 9,948 ms | 59.2s | 0.948x | ❌ Worse |
| 5 | JOIN_GLOBAL_BLOOM=1 | 9,668 ms | 61.0s | 0.975x | ❌ Slower |
| 6 | REQ_BUILD_FROM_PAGES=1 | 9,492 ms | 60.6s | 0.993x | Negligible |
| 7 | JOIN_GLOBAL_BLOOM_BITS=16 | 10,251 ms | 62.7s | 0.920x | ❌ Slower |
| 8 | JOIN_GLOBAL_BLOOM=1 + BITS=8 | 10,071 ms | 59.1s | 0.936x | ❌ Worse |
| 9 | AUTO_BUILD_SIDE=1 (explicit) | 9,448 ms | 62.5s | 0.998x | Negligible |
| 10 | AUTO_BUILD_SIDE=1 + JOIN_GLOBAL_BLOOM=0 | 10,352 ms | 60.3s | 0.911x | ❌ Slower |

---

## 🎯 Key Findings

### ✅ **FASTEST Configuration: Test #2**
```bash
JOIN_TELEMETRY=1 ./build/fast plans.json
```
- **Query Runtime:** 9,280 ms (9.28 seconds)
- **Wall Clock Time:** 60.1 seconds
- **Speedup vs. Default:** 1.6% faster
- **Verdict:** Marginal improvement; telemetry has minimal overhead

### ⚡ **Recommended Configuration: Test #1 (Default)**
```bash
./build/fast plans.json
```
- **Query Runtime:** 9,425 ms
- **Wall Clock Time:** 59.4 seconds
- **Verdict:** **Best overall.** The default configuration (AUTO_BUILD_SIDE=1 implicitly enabled) is already optimized.

---

## 📈 Performance Analysis

### Parameters Tested:
1. **JOIN_TELEMETRY** - Memory bandwidth telemetry tracking
   - Effect: +1.6% faster (minimal overhead)
   
2. **AUTO_BUILD_SIDE** - Automatic build-side selection (smaller table)
   - Default: ENABLED (1)
   - Disabling (0): -3.2% slower (avoids heuristic)
   
3. **JOIN_GLOBAL_BLOOM** - Global Bloom filter across all joins
   - Default: DISABLED (0)
   - Enabling (1): -2.5% slower (extra filtering cost)
   
4. **REQ_BUILD_FROM_PAGES** - Build hashtable from columnar pages
   - Default: varies
   - Effect: -0.7% slower (negligible)
   
5. **JOIN_GLOBAL_BLOOM_BITS** - Bloom filter size (default 4)
   - Increasing to 16: -8.0% slower (diminishing returns)
   - Combination with BLOOM=1: -6.4% slower

---

## 🎨 Optimization Impact

### What's Already Optimized (Default):
✅ **Unchained Hashing** - O(1) probe with Bloom filters per bucket
✅ **Late Materialization** - Zero-copy string refs (PackedStringRef)
✅ **Column-Store Storage** - Columnar format for efficient scans
✅ **Auto Build-Side Selection** - Smaller table builds hash table

### What Slows Things Down:
❌ Global Bloom filters (extra memory & computation)
❌ Disabling auto build-side heuristic (suboptimal join order)
❌ Increased Bloom filter bits (diminishing returns)

---

## 💡 Conclusion

**The default configuration is near-optimal for this workload.**

- **Baseline Query Runtime:** 9,425 ms
- **Best Found:** 9,280 ms (1.6% improvement with JOIN_TELEMETRY)
- **Improvement Potential:** Minimal (<2%)

The system is already well-tuned with:
- Efficient unchained hashing with per-bucket Bloom filters
- Late materialization reducing memory overhead
- Columnar storage for selective column scans
- Smart build-side selection heuristics

**Recommendation:** Use the **default configuration** for production as it offers the best balance of speed and simplicity.

---

## 📝 Test Commands Used

```bash
# Test 1: Default
./build/fast plans.json

# Test 2: With telemetry
JOIN_TELEMETRY=1 ./build/fast plans.json

# Test 3-10: Various combinations
JOIN_GLOBAL_BLOOM=1 ./build/fast plans.json
AUTO_BUILD_SIDE=0 ./build/fast plans.json
# ... etc
```
