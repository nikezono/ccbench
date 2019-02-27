
#include <string.h>

#include <atomic>
#include <algorithm>
#include <bitset>

#include "../include/debug.hpp"
#include "../include/tsc.hpp"

#include "include/common.hpp"
#include "include/transaction.hpp"
#include "include/version.hpp"

extern bool chkClkSpan(uint64_t &start, uint64_t &stop, uint64_t threshold);

using namespace std;

inline
SetElement *
TxExecutor::searchReadSet(unsigned int key) 
{
  for (auto itr = readSet.begin(); itr != readSet.end(); ++itr) {
    if ((*itr).key == key) return &(*itr);
  }

  return nullptr;
}

inline
SetElement *
TxExecutor::searchWriteSet(unsigned int key) 
{
  for (auto itr = writeSet.begin(); itr != writeSet.end(); ++itr) {
    if ((*itr).key == key) return &(*itr);
  }

  return nullptr;
}

void
TxExecutor::tbegin()
{
  TransactionTable *newElement, *tmt;

  tmt = __atomic_load_n(&TMT[thid], __ATOMIC_ACQUIRE);
  if (this->status == TransactionStatus::aborted) {
    this->txid = tmt->lastcstamp.load(memory_order_acquire);
    newElement = new TransactionTable(0, 0, UINT32_MAX, tmt->lastcstamp.load(std::memory_order_acquire), TransactionStatus::inFlight);
  }
  else {
    this->txid = this->cstamp;
    newElement = new TransactionTable(0, 0, UINT32_MAX, tmt->cstamp.load(std::memory_order_acquire), TransactionStatus::inFlight);
  }

  for (unsigned int i = 1; i < THREAD_NUM; ++i) {
    if (i == thid) continue;
    do {
      tmt = __atomic_load_n(&TMT[i], __ATOMIC_ACQUIRE);
    } while (tmt == nullptr);
    this->txid = max(this->txid, tmt->lastcstamp.load(memory_order_acquire));
  }
  this->txid += 1;
  newElement->txid = this->txid;

  TransactionTable *expected, *desired;
  tmt = __atomic_load_n(&TMT[thid], __ATOMIC_ACQUIRE);
  expected = tmt;
  gcobject.gcqForTMT.push(expected);
  for (;;) {
    desired = newElement;
    if (__atomic_compare_exchange_n(&TMT[thid], &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
  }
  
  pstamp = 0;
  sstamp = UINT32_MAX;
  status = TransactionStatus::inFlight;
}

char *
TxExecutor::ssn_tread(unsigned int key)
{
  //safe retry property
  
  //if it already access the key object once.
  // w
  SetElement *inW = searchWriteSet(key);
  if (inW) return writeVal;
  // tanabe の実験ではスレッドごとに書き込む新しい値が決まっている．

  SetElement *inR = searchReadSet(key);
  if (inR) {
    return inR->ver->val;
  }

  // if v not in t.writes:
  Version *ver = Table[key].latest.load(std::memory_order_acquire);
  if (ver->status.load(memory_order_acquire) != VersionStatus::committed) {
    ver = ver->committed_prev;
  }
  uint64_t verCstamp = ver->cstamp.load(memory_order_acquire);
  while (txid < (verCstamp >> TIDFLAG)) {
    //printf("txid %d, (verCstamp >> 1) %d\n", txid, verCstamp >> 1);
    //fflush(stdout);
    ver = ver->committed_prev;
    if (ver == NULL) {
      cout << "txid : " << txid
        << ", verCstamp : " << verCstamp << endl;
      ERR;
    }
    verCstamp = ver->cstamp.load(memory_order_acquire);
  }

  if (ver->psstamp.atomicLoadSstamp() == (UINT32_MAX & ~(TIDFLAG)))
    // no overwrite yet
    readSet.emplace_back(key, ver);
  else 
    // update pi with r:w edge
    this->sstamp = min(this->sstamp, (ver->psstamp.atomicLoadSstamp() >> TIDFLAG));
  upReadersBits(ver);

  verify_exclusion_or_abort();

  // for fairness
  // ultimately, it is wasteful in prototype system.
  memcpy(returnVal, ver->val, VAL_SIZE);
  return ver->val;
}

void
TxExecutor::ssn_twrite(unsigned int key)
{
  // if it already wrote the key object once.
  SetElement *inW = searchWriteSet(key);
  if (inW) return;
  // tanabe の実験では，スレッドごとに書き込む新しい値が決まっている．
  // であれば，コミットが確定し，version status を committed にして other worker に visible になるときに
  // memcpy するのが一番無駄が少ない．

  // if v not in t.writes:
  //
  //first-updater-wins rule
  //Forbid a transaction to update  a record that has a committed head version later than its begin timestamp.
  
  Version *expected, *desired;
  desired = new Version();
  uint64_t tmptid = this->thid;
  tmptid = tmptid << 1;
  tmptid |= 1;
  desired->cstamp.store(tmptid, memory_order_release);  // read operation, write operation, ガベコレからアクセスされるので， CAS 前に格納

  Version *vertmp;
  expected = Table[key].latest.load(std::memory_order_acquire);
  for (;;) {
    // w-w conflict
    // first updater wins rule
    if (expected->status.load(memory_order_acquire) == VersionStatus::inFlight) {
      this->status = TransactionStatus::aborted;
      TransactionTable *tmt = __atomic_load_n(&TMT[thid], __ATOMIC_ACQUIRE);
      tmt->status.store(TransactionStatus::aborted, memory_order_release);
      delete desired;
      return;
    }
    
    // 先頭バージョンが committed バージョンでなかった時
    vertmp = expected;
    while (vertmp->status.load(memory_order_acquire) != VersionStatus::committed) {
      vertmp = vertmp->committed_prev;
      if (vertmp == nullptr) ERR;
    }

    // vertmp は commit済み最新バージョン
    uint64_t verCstamp = vertmp->cstamp.load(memory_order_acquire);
    if (txid < (verCstamp >> 1)) {  
      //  write - write conflict, first-updater-wins rule.
      // Writers must abort if they would overwirte a version created after their snapshot.
      this->status = TransactionStatus::aborted;
      TransactionTable *tmt = __atomic_load_n(&TMT[thid], __ATOMIC_ACQUIRE);
      tmt->status.store(TransactionStatus::aborted, memory_order_release);
      delete desired;
      return;
    }

    desired->prev = expected;
    desired->committed_prev = vertmp;
    if (Table[key].latest.compare_exchange_strong(expected, desired, memory_order_acq_rel, memory_order_acquire)) break;
  }

  //ver->committed_prev->sstamp に TID を入れる
  uint64_t tmpTID = thid;
  tmpTID = tmpTID << 1;
  tmpTID |= 1;
  desired->committed_prev->psstamp.atomicStoreSstamp(tmpTID);

  // update eta with w:r edge
  this->pstamp = max(this->pstamp, desired->committed_prev->psstamp.atomicLoadPstamp());
  writeSet.emplace_back(key, desired);
  
  //  avoid false positive
  auto itr = readSet.begin();
  while (itr != readSet.end()) {
    if ((*itr).key == key) {
      readSet.erase(itr);
      downReadersBits(vertmp);
      break;
    } else itr++;
  }
  
  verify_exclusion_or_abort();
}

void
TxExecutor::ssn_commit()
{
  this->status = TransactionStatus::committing;
  TransactionTable *tmt = __atomic_load_n(&TMT[thid], __ATOMIC_ACQUIRE);
  tmt->status.store(TransactionStatus::committing);

  this->cstamp = ++Lsn;
  tmt->cstamp.store(this->cstamp, memory_order_release);
  
  // begin pre-commit
  SsnLock.lock();

  // finalize eta(T)
  for (auto itr = writeSet.begin(); itr != writeSet.end(); ++itr) {
    pstamp = max(pstamp, (*itr).ver->committed_prev->psstamp.atomicLoadPstamp());
  } 

  // finalize pi(T)
  sstamp = min(sstamp, cstamp);
  for (auto itr = readSet.begin(); itr != readSet.end(); ++itr) {
    uint32_t verSstamp = (*itr).ver->psstamp.atomicLoadSstamp();
    // if the lowest bit raise, the record was overwrited by other concurrent transactions.
    // but in serial SSN, the validation of the concurrent transactions will be done after that of this transaction.
    // So it can skip.
    // And if the lowest bit raise, the stamp is TID;
    if (verSstamp & 1) continue;
    sstamp = min(sstamp, verSstamp >> 1);
  }

  // ssn_check_exclusion
  if (pstamp < sstamp) status = TransactionStatus::committed;
  else {
    status = TransactionStatus::aborted;
    SsnLock.unlock();
    return;
  }

  // update eta
  for (auto itr = readSet.begin(); itr != readSet.end(); ++itr) {
    (*itr).ver->psstamp.atomicStorePstamp(max((*itr).ver->psstamp.atomicLoadPstamp(), cstamp));
    // down readers bit
    downReadersBits((*itr).ver);
  }

  // update pi
  uint64_t verSstamp = this->sstamp;
  verSstamp = verSstamp << 1;
  verSstamp &= ~(1);

  uint64_t verCstamp = cstamp;
  verCstamp = verCstamp << 1;
  verCstamp &= ~(1);

  for (auto itr = writeSet.begin(); itr != writeSet.end(); ++itr) {
    (*itr).ver->committed_prev->psstamp.atomicStoreSstamp(verSstamp);
    // initialize new version
    (*itr).ver->psstamp.atomicStorePstamp(cstamp);
    (*itr).ver->cstamp = verCstamp;
  }

  //logging
  //?*
  
  // status, inFlight -> committed
  for (auto itr = writeSet.begin(); itr != writeSet.end(); ++itr) {
    memcpy((*itr).ver->val, writeVal, VAL_SIZE);
    (*itr).ver->status.store(VersionStatus::committed, memory_order_release);
  }

  this->status = TransactionStatus::committed;
  SsnLock.unlock();
  readSet.clear();
  writeSet.clear();
  return;
}

void
TxExecutor::ssn_parallel_commit()
{
  this->status = TransactionStatus::committing;
  TransactionTable *tmt = __atomic_load_n(&TMT[thid], __ATOMIC_ACQUIRE);
  tmt->status.store(TransactionStatus::committing);

  this->cstamp = ++Lsn;
  //this->cstamp = 2; test for effect of centralized counter in YCSB-C (read only workload)

  tmt->cstamp.store(this->cstamp, memory_order_release);
  
  // begin pre-commit
  
  // finalize pi(T)
  this->sstamp = min(this->sstamp, this->cstamp);
  for (auto itr = readSet.begin(); itr != readSet.end(); ++itr) {
    uint32_t v_sstamp = (*itr).ver->psstamp.atomicLoadSstamp();
    // if lowest bits raise, it is TID
    if (v_sstamp & TIDFLAG) {
      // identify worker by using TID
      uint8_t worker = (v_sstamp >> TIDFLAG);
      if (worker == thid) {
        // it mustn't occur "worker == thid", so it's error.
        dispWS();
        dispRS();
        ERR;
      }
      // もし worker が inFlight (read phase 実行中) だったら
      // このトランザクションよりも新しい commit timestamp でコミットされる．
      // 従って pi となりえないので，スキップ．
      // そうでないなら，チェック
      /*cout << "tmt : " << tmt << endl;
      cout << "worker : " << worker << endl;
      cout << "v_sstamp : " << v_sstamp << endl;
      cout << "UINT32_MAX : " << UINT32_MAX << endl;*/
      tmt = __atomic_load_n(&TMT[worker], __ATOMIC_ACQUIRE);
      if (tmt->status.load(memory_order_acquire) == TransactionStatus::committing) {
        // worker は ssn_parallel_commit() に突入している
        // cstamp が確実に取得されるまで待機 (cstamp は初期値 0)
        while (tmt->cstamp.load(memory_order_acquire) == 0);
        if (tmt->status.load(memory_order_acquire) == TransactionStatus::aborted) continue;

        // worker->cstamp が this->cstamp より小さいなら pi になる可能性あり
        if (tmt->cstamp.load(memory_order_acquire) < this->cstamp) {
          // parallel_commit 終了待ち（sstamp確定待ち)
          while (tmt->status.load(memory_order_acquire) == TransactionStatus::committing);
          if (tmt->status.load(memory_order_acquire) == TransactionStatus::committed) {
            this->sstamp = min(this->sstamp, tmt->sstamp.load(memory_order_acquire));
          }
        }
      }
    } else {
      this->sstamp = min(this->sstamp, v_sstamp >> TIDFLAG);
    }
  }

  // finalize eta
  uint64_t one = 1;
  for (auto itr = writeSet.begin(); itr != writeSet.end(); ++itr) {

    //for r in v.prev.readers
    uint64_t rdrs = (*itr).ver->readers.load(memory_order_acquire);
    for (unsigned int worker = 1; worker <= THREAD_NUM; ++worker) {
      if ((rdrs & (one << worker)) ? 1 : 0) {
        tmt = __atomic_load_n(&TMT[worker], __ATOMIC_ACQUIRE);
        // 並行 reader が committing なら無視できない．
        // inFlight なら無視できる．なぜならそれが commit する時にはこのトランザクションよりも
        // 大きい cstamp を有し， eta となりえないから．
        while (tmt->status.load(memory_order_acquire) == TransactionStatus::committing) {
          while (tmt->cstamp.load(memory_order_acquire) == 0);
          if (tmt->status.load(memory_order_acquire) == TransactionStatus::aborted) continue;
          
          // worker->cstamp が this->cstamp より小さいなら eta になる可能性あり
          if (tmt->cstamp.load(memory_order_acquire) < this->cstamp) {
            // parallel_commit 終了待ち (sstamp 確定待ち)
            while (tmt->status.load(memory_order_acquire) == TransactionStatus::committing);
            if (tmt->status.load(memory_order_acquire) == TransactionStatus::committed) {
              this->pstamp = min(this->pstamp, tmt->cstamp.load(memory_order_acquire));
            }
          }
        }
      } 
    }
    // re-read pstamp in case we missed any reader
    this->pstamp = min(this->pstamp, (*itr).ver->committed_prev->psstamp.atomicLoadPstamp());
  }

  tmt = __atomic_load_n(&TMT[thid], __ATOMIC_ACQUIRE);
  // ssn_check_exclusion
  if (pstamp < sstamp) {
    status = TransactionStatus::committed;
    tmt->sstamp.store(this->sstamp, memory_order_release);
    tmt->status.store(TransactionStatus::committed, memory_order_release);
  } else {
    status = TransactionStatus::aborted;
    tmt->status.store(TransactionStatus::aborted, memory_order_release);
    return;
  }

  // update eta
  for (auto itr = readSet.begin(); itr != readSet.end(); ++itr) {
    Psstamp pstmp;
    pstmp.obj = (*itr).ver->psstamp.atomicLoad();
    while (pstmp.pstamp < this->cstamp) {
      if ((*itr).ver->psstamp.atomicCASPstamp(pstmp.pstamp, this->cstamp)) break;
      pstmp.obj = (*itr).ver->psstamp.atomicLoad();
    }
    downReadersBits((*itr).ver);
  }


  // update pi
  uint64_t verSstamp = this->sstamp;
  verSstamp = verSstamp << TIDFLAG;
  verSstamp &= ~(TIDFLAG);

  uint64_t verCstamp = cstamp;
  verCstamp = verCstamp << TIDFLAG;
  verCstamp &= ~(TIDFLAG);

  for (auto itr = writeSet.begin(); itr != writeSet.end(); ++itr) {
    (*itr).ver->committed_prev->psstamp.atomicStoreSstamp(verSstamp);
    (*itr).ver->cstamp.store(verCstamp, memory_order_release);
    (*itr).ver->psstamp.atomicStorePstamp(this->cstamp);
    (*itr).ver->psstamp.atomicStoreSstamp(UINT32_MAX & ~(TIDFLAG));
    memcpy((*itr).ver->val, writeVal, VAL_SIZE);
    (*itr).ver->status.store(VersionStatus::committed, memory_order_release);
    gcobject.gcqForVersion.push(GCElement((*itr).key, (*itr).ver, verCstamp));
  }

  //logging
  //?*

  readSet.clear();
  writeSet.clear();
  ++rsobject.localCommitCounts;
  return;
}

void
TxExecutor::abort()
{
  for (auto itr = writeSet.begin(); itr != writeSet.end(); ++itr) {
    (*itr).ver->committed_prev->psstamp.atomicStoreSstamp(UINT32_MAX & ~(TIDFLAG));
    (*itr).ver->status.store(VersionStatus::aborted, memory_order_release);
    gcobject.gcqForVersion.push(GCElement((*itr).key, (*itr).ver, this->txid << TIDFLAG));
  }
  writeSet.clear();

  for (auto itr = readSet.begin(); itr != readSet.end(); ++itr)
    downReadersBits((*itr).ver);

  readSet.clear();
  ++rsobject.localAbortCounts;
}

void
TxExecutor::verify_exclusion_or_abort()
{
  if (this->pstamp >= this->sstamp) {
    this->status = TransactionStatus::aborted;
    TransactionTable *tmt = __atomic_load_n(&TMT[thid], __ATOMIC_ACQUIRE);
    tmt->status.store(TransactionStatus::aborted, memory_order_release);
  }
}

void
TxExecutor::mainte()
{
  gcstop = rdtsc();
  if (chkClkSpan(gcstart, gcstop, GC_INTER_US * CLOCK_PER_US)) {
    uint32_t loadThreshold = gcobject.getGcThreshold();
    if (preGcThreshold != loadThreshold) {
      gcobject.gcTMTelement(rsobject);
      gcobject.gcVersion(rsobject);
      preGcThreshold = loadThreshold;

      ++rsobject.localGCCounts;
      gcstart = gcstop;
    }
  }
}

void
TxExecutor::dispWS()
{
  cout << "th " << this->thid << " : write set : ";
  for (auto itr = writeSet.begin(); itr != writeSet.end(); ++itr) {
    cout << "(" << (*itr).key << ", " << (*itr).ver->val << "), ";
  }
  cout << endl;
}

void
TxExecutor::dispRS()
{
  cout << "th " << this->thid << " : read set : ";
  for (auto itr = readSet.begin(); itr != readSet.end(); ++itr) {
    cout << "(" << (*itr).key << ", " << (*itr).ver->val << "), ";
  }
  cout << endl;
}
