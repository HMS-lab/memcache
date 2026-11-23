# Frequent Master Switches in HA Mode

## Symptoms

In HA mode, the master repeatedly switches between master and backup roles. The master initialization logs include:

```ini
2026-06-02 08:57:07.344376 20 warning [MMC_leader_election.cpp:26 Logger] Updated label of self.pod_name=grtest-mmc-1-mmc-meta-0 to labels={'role': 'master'}
2026-06-02 08:57:07.344405 20 debug [MMC_leader_election.cpp:105 ElectionLoop] Election loop circle, pod-grtest-mmc-1-mmc-meta-0, times=33
2026-06-02 08:57:08.832026 24 warning [MMC_leader_election.cpp:189 RenewLease] Renew loop acquired Python GIL, pod-grtest-mmc-1-mmc-meta-0
2026-06-02 08:57:08.848462 24 debug [MMC_leader_election.cpp:206 RenewLease] Succeed in updating lease-grtest-mmc-1-mmc-meta-Lease: curHolder=grtest-mmc-1-mmc-meta-0, leaseDuration=30, renewTime=2026-06-02 08:57:38.848462, current=2026-06-02 08:57:38.848462, retry period=30
2026-06-02 08:57:10.344572 20 debug [MMC_leader_election.cpp:105 ElectionLoop] Election loop circle, pod-grtest-mmc-1-mmc-meta-0, times=64
2026-06-02 08:57:10.344681 20 debug [MMC_leader_election.cpp:107 ElectionLoop] Election loop acquired Python GIL, pod-grtest-mmc-1-mmc-meta-0
2026-06-02 08:57:10.344710 20 debug [MMC_leader_election.cpp:147 CheckLeaderStatus] Pod grtest-mmc-1-mmc-meta-0 labels={'role': 'master'}
2026-06-02 08:57:10.360390 20 warning [MMC_leader_election.cpp:26 Logger] Updated label of self.pod_name=grtest-mmc-1-mmc-meta-0 to labels={'role': 'backup'}
```

## Root Causes

Common causes include:

### 1. 🔥 Renewal failure → demotion → detection of self as holder → promotion → repeat

- RenewLoop renews the lease every 3 seconds. If network instability, a slow API server, or an HTTP 409 conflict causes renewal to fail, `isLeader_` becomes false and the label changes to backup immediately.
- ElectionLoop in the same process also checks the lease every 3 seconds. If it is still the holder, it immediately reclaims leadership and changes the label to master.
- The label can switch back and forth within 3 seconds.

**Solution**

Check cluster networking or increase `leaseDurationSeconds` from 10 to 20–30.

### 2. 🔥 Node clocks are not synchronized

- `check_leader_status` uses local time to determine whether a lease has expired.
- If A is 2 seconds ahead of B, B may treat the lease as expired and claim leadership.
- A still considers the lease valid, causing a conflict.
- The deployment guide requires manual time setting, which can leave clock differences in production.

**Solution**

Manually synchronize the pod clocks.

### 3. 🔥 An external controller clears Kubernetes lease information

Most cases are caused by 1 or 2. Otherwise, user code that periodically clears Kubernetes leases is a likely cause.

**Solution**

Investigate and fix the external lease cleanup.
