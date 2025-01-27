#include <atomic>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <xmmintrin.h>

#include "../include/zipf.hh"

#define TIDFLAG 1
#define CACHE_LINE_SIZE 64
#define PAGE_SIZE 4096
#define clocks_per_us 2100   //"CPU_MHz. Use this info for measuring time."
#define extime 3             // Execution time[sec].
#define max_ope 10           // Total number of operations per single transaction.
#define max_ope_readonly 100 // read only transactionの長さ
#define ronly_ratio 40       // read-only transaction rate
#define rratio 50            // read ratio of single transaction.
#define thread_num 10        // Total number of worker threads.
#define tuple_num 100        //"Total number of records."
#define use_lock 1           // 0 = dont use read-only lock, 1 = use read-only lock

using namespace std;

class Result
{
public:
    uint64_t local_abort_counts_ = 0;
    uint64_t local_commit_counts_ = 0;
    uint64_t total_abort_counts_ = 0;
    uint64_t total_commit_counts_ = 0;
    uint64_t local_readphase_counts_ = 0;
    uint64_t local_writephase_counts_ = 0;
    uint64_t local_commitphase_counts_ = 0;
    uint64_t local_wwconflict_counts_ = 0;
    uint64_t total_readphase_counts_ = 0;
    uint64_t total_writephase_counts_ = 0;
    uint64_t total_commitphase_counts_ = 0;
    uint64_t total_wwconflict_counts_ = 0;
    uint64_t local_uselock_counts_ = 0;
    uint64_t total_uselock_counts_ = 0;

    void displayAllResult()
    {
        cout << "abort_counts_:\t\t\t" << total_abort_counts_ << endl;
        cout << "commit_counts_:\t\t\t" << total_commit_counts_ << endl;
        cout << "uselock_counts_:\t\t" << total_uselock_counts_ << endl;
        cout << "read SSNcheck abort:\t\t" << total_readphase_counts_ << endl;
        cout << "write SSNcheck abort:\t\t" << total_writephase_counts_ << endl;
        cout << "commit SSNcheck abort:\t\t" << total_commitphase_counts_ << endl;
        cout << "ww conflict abort:\t\t" << total_wwconflict_counts_ << endl;
        // displayAbortRate
        long double ave_rate =
            (double)total_abort_counts_ /
            (double)(total_commit_counts_ + total_abort_counts_);
        cout << fixed << setprecision(4) << "abort_rate:\t\t\t" << ave_rate << endl;
        // displayTps
        uint64_t result = total_commit_counts_ / extime;
        cout << "latency[ns]:\t\t\t" << powl(10.0, 9.0) / result * thread_num
             << endl;
        cout << "throughput[tps]:\t\t" << result << endl;
    }

    void addLocalAllResult(const Result &other)
    {
        total_abort_counts_ += other.local_abort_counts_;
        total_commit_counts_ += other.local_commit_counts_;
        total_uselock_counts_ += other.local_uselock_counts_;
        total_readphase_counts_ += other.local_readphase_counts_;
        total_writephase_counts_ += other.local_writephase_counts_;
        total_commitphase_counts_ += other.local_commitphase_counts_;
        total_wwconflict_counts_ += other.local_wwconflict_counts_;
    }
};

bool isReady(const std::vector<char> &readys)
{
    for (const char &b : readys)
    {
        if (!b)
            return false;
    }
    return true;
}

void waitForReady(const std::vector<char> &readys)
{
    while (!isReady(readys))
    {
        _mm_pause();
    }
}

std::vector<Result> ErmiaResult;

void initResult() { ErmiaResult.resize(thread_num); }

std::atomic<uint64_t> timestampcounter(1); // timestampを割り当てる

enum class VersionStatus : uint8_t
{
    inFlight,
    committed,
    aborted,
};

class Version
{
public:
    uint32_t pstamp_; // Version access stamp, eta(V),
    uint32_t sstamp_; // Version successor stamp, pi(V)
    Version *prev_;   // Pointer to overwritten version
    uint32_t cstamp_; // Version creation stamp, c(V)
    VersionStatus status_;
    uint64_t val_;

    Version() {}
};

class RWLock
{
public:
    uint64_t counter = 0;
    // uint32_t sstamp_ = UINT32_MAX;
    vector<uint32_t> pstamp_table_;
    RWLock() {}

    void r_getlock(uint32_t pstamp)
    {
        if (counter >= 0)
        {
            counter++;
            pstamp_table_.emplace_back(pstamp);
            return;
        }
        else
        {
            r_getlock(pstamp);
            // return false;
        }
    }

    void r_unlock(uint32_t pstamp)
    {
        if (counter > 0)
        {
            counter--;
            for (auto itr = pstamp_table_.begin(); itr != pstamp_table_.end();)
            {
                if (*itr == pstamp)
                {
                    itr = pstamp_table_.erase(itr);
                    cout << "ok" << endl;
                }
                else
                {
                    itr++;
                }
            }
        }
    }

    bool w_trylock()
    {
        if (counter == 0)
        {
            counter--;
            return true;
        }
        else
        {
            return false;
        }
    }

    bool upgrade_lock(uint64_t pstamp)
    {
        for (int i = 0; i < pstamp_table_.size(); i++)
        {
            if (pstamp_table_.at(i) <= pstamp) // この条件が間違っていると思われる
            {
                return false;
            }
        }
        return true;
    }

    void w_getlock(uint64_t sstamp)
    {
        if (counter == 0)
        {
            counter--;
        }
        else if (counter < 0)
        {
            w_getlock(sstamp);
        }
        else
        {
            if (upgrade_lock(sstamp) == true)
            {
                counter = -1;
            }
            else
            {
                w_getlock(sstamp);
            }
        }
    }

    void w_unlock()
    {
        counter++;
    }
};

class Tuple
{
public:
    Version *latest_;
    uint64_t key;
    RWLock lock_; // 提案手法用

    Tuple() {}
};

enum class TransactionStatus : uint8_t
{
    inFlight,
    committing,
    committed,
    aborted,
};

Tuple *Table;       // databaseのtable
std::mutex SsnLock; // giant lock

enum class Ope : uint8_t
{
    READ,
    WRITE,
};

class Operation
{
public:
    uint64_t key_;
    Version *ver_;

    Operation(uint64_t key, Version *ver) : key_(key), ver_(ver) {}
};

class Task
{
public:
    Ope ope_;
    uint64_t key_;
    uint64_t write_val_;

    Task(Ope ope, uint64_t key) : ope_(ope), key_(key) {}
    Task(Ope ope, uint64_t key, uint64_t write_val) : ope_(ope), key_(key), write_val_(write_val) {}
};

void viewtask(vector<Task> &tasks)
{
    for (auto itr = tasks.begin(); itr != tasks.end(); itr++)
    {
        if (itr->ope_ == Ope::READ)
        {
            cout << "R" << itr->key_ << " ";
        }
        else
        {
            cout << "W" << itr->key_ << " ";
        }
    }
    cout << endl;
}

void makeTask(std::vector<Task> &tasks, Xoroshiro128Plus &rnd, FastZipf &zipf)
{
    tasks.clear();
    if ((zipf() % 100) < ronly_ratio)
    {
        for (size_t i = 0; i < max_ope_readonly; ++i)
        {
            uint64_t tmpkey;
            // decide access destination key.
            tmpkey = zipf() % tuple_num;
            tasks.emplace_back(Ope::READ, tmpkey);
        }
    }
    else
    {
        for (size_t i = 0; i < max_ope; ++i)
        {
            uint64_t tmpkey;
            // decide access destination key.
            tmpkey = zipf() % tuple_num;
            // decide operation type.
            if ((rnd.next() % 100) < rratio)
            {
                tasks.emplace_back(Ope::READ, tmpkey);
            }
            else
            {
                tasks.emplace_back(Ope::WRITE, tmpkey, zipf());
            }
        }
    }
}

class Transaction
{
public:
    uint64_t write_val_;
    uint8_t thid_;                 // thread ID
    uint32_t cstamp_ = 0;          // Transaction end time, c(T)
    uint32_t pstamp_ = 0;          // Predecessor high-water mark, η (T)
    uint32_t sstamp_ = UINT32_MAX; // Successor low-water mark, pi (T)
    uint32_t txid_;                // TID and begin timestamp
    TransactionStatus status_ = TransactionStatus::inFlight;
    bool lock_flag = false; // lockを持っているか判定

    vector<Operation> read_set_;  // write set
    vector<Operation> write_set_; // read set
    vector<Task> task_set_;       // 生成されたtransaction

    Result *eres_;

    Transaction() {}

    Transaction(uint8_t thid, Result *eres) : thid_(thid), eres_(eres)
    {
        read_set_.reserve(max_ope_readonly);
        write_set_.reserve(max_ope);
        task_set_.reserve(max_ope_readonly);
    }

    bool searchReadSet(unsigned int key)
    {
        for (auto itr = read_set_.begin(); itr != read_set_.end(); ++itr)
        {
            if ((*itr).key_ == key)
                return true;
        }
        return false;
    }

    bool searchWriteSet(unsigned int key)
    {
        for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr)
        {
            if ((*itr).key_ == key)
                return true;
        }
        return false;
    }

    void tbegin()
    {
        // -------------------------------------------------------------------------------------
        if (this->status_ == TransactionStatus::aborted && this->lock_flag == true)
        {
            this->txid_ = this->cstamp_;
        }
        else
        {
            this->txid_ = atomic_fetch_add(&timestampcounter, 1);
        }
        // -------------------------------------------------------------------------------------
        // this->txid_ = atomic_fetch_add(&timestampcounter, 1);
        this->cstamp_ = 0;
        pstamp_ = 0;
        sstamp_ = UINT32_MAX;
        status_ = TransactionStatus::inFlight;
    }

    void ssn_tread(uint64_t key)
    {
        // read-own-writes, re-read from previous read in the same tx.
        if (searchWriteSet(key) == true || searchReadSet(key) == true)
            goto FINISH_TREAD;

        // get version to read
        Tuple *tuple;
        tuple = get_tuple(key);
        Version *ver;
        ver = tuple->latest_;
        while (ver->status_ != VersionStatus::committed || txid_ < ver->cstamp_)
            ver = ver->prev_;

        // update eta(t) with w:r edges
        this->pstamp_ = max(this->pstamp_, ver->cstamp_);

        if (ver->sstamp_ == (UINT32_MAX))
        { //// no overwrite yet
            read_set_.emplace_back(key, ver);
        }
        else
        {
            // update pi with r:w edge
            this->sstamp_ = min(this->sstamp_, ver->sstamp_);
        }
        // if (use_lock == 0)
        verify_exclusion_or_abort();
        if (this->status_ == TransactionStatus::aborted)
        {
            ++eres_->local_readphase_counts_;
            goto FINISH_TREAD;
        }
    FINISH_TREAD:
        return;
    }

    void ssn_twrite(uint64_t key)
    {
        // update local write set
        if (searchWriteSet(key) == true)
            return;

        Tuple *tuple;
        tuple = get_tuple(key);

        // If v not in t.writes:
        Version *expected, *tmp;
        tmp = new Version();
        tmp->cstamp_ = this->txid_;
        tmp->status_ = VersionStatus::inFlight;
        tmp->pstamp_ = 0;
        tmp->sstamp_ = UINT32_MAX;

        Version *vertmp;
        expected = tuple->latest_;
        uint64_t sstampforabort;

        // no wait
        /*if (tuple->lock_.w_trylock() == false)
        {
            this->status_ = TransactionStatus::aborted;
            // cout << "abort by ww1" << endl;
            ++eres_->local_wwconflict_counts_;
            goto FINISH_TWRITE;
        }*/

        // wait  die
        if (tuple->lock_.w_trylock() == false)
        {
            if (tuple->latest_->status_ == VersionStatus::inFlight)
            {
                if (this->txid_ <= tuple->latest_->cstamp_)
                {
                    this->status_ = TransactionStatus::aborted;
                    ++eres_->local_wwconflict_counts_;
                    goto FINISH_TWRITE;
                }
                else
                {
                    // lockが取れるまで待つ
                    tuple->lock_.w_getlock(this->sstamp_);
                }
            }
            else
            {
                vertmp = tuple->latest_;
                while (vertmp->status_ != VersionStatus::committed)
                {
                    vertmp = vertmp->prev_;
                }
                if (txid_ < vertmp->cstamp_)
                {
                    this->status_ = TransactionStatus::aborted;
                    ++eres_->local_wwconflict_counts_;
                    goto FINISH_TWRITE;
                }
                else
                {
                    // abortしてlockを解放していない or read lockのせいでlockが取得できない
                    tuple->lock_.w_getlock(this->sstamp_);
                }
            }
        }

        // first updater wins
        // Forbid a transaction to update a record that has a committed version later than its begin timestamp.
        /*if (expected->status_ == VersionStatus::inFlight)
        {
            if (this->txid_ <= expected->cstamp_)
            {
                this->status_ = TransactionStatus::aborted;
                ++eres_->local_wwconflict_counts_;
                goto FINISH_TWRITE;
            }
        }

        // if latest version is not comitted, vertmp is latest committed version.
        vertmp = expected;
        while (vertmp->status_ != VersionStatus::committed)
        {
            vertmp = vertmp->prev_;
        }

        if (txid_ < vertmp->cstamp_)
        {
            // Writers must abort if they would overwirte a version created after their snapshot.
            this->status_ = TransactionStatus::aborted;
            ++eres_->local_wwconflict_counts_;
            goto FINISH_TWRITE;
        }*/

        // Update eta with w:r edge
        this->pstamp_ = max(this->pstamp_, tmp->prev_->pstamp_);

        verify_exclusion_or_abort();
        if (this->status_ == TransactionStatus::aborted)
        {
            tuple->latest_ = expected;
            ++eres_->local_writephase_counts_;
            goto FINISH_TWRITE;
        }
        // pointer処理
        tmp->prev_ = expected;
        tuple->latest_ = tmp;
        write_set_.emplace_back(key, tmp); // t.writes.add(V)

    FINISH_TWRITE:
        if (this->status_ == TransactionStatus::aborted)
        {
            delete tmp;
        }
        return;
    }

    void
    ssn_commit()
    {
        this->status_ = TransactionStatus::committing;
        this->cstamp_ = atomic_fetch_add(&timestampcounter, 1);

        // begin pre-commit
        SsnLock.lock();

        // finalize pi(T)
        this->sstamp_ = min(this->sstamp_, this->cstamp_);
        for (auto itr = read_set_.begin(); itr != read_set_.end(); ++itr)
        {
            this->sstamp_ = min(this->sstamp_, (*itr).ver_->sstamp_);
        }

        // finalize eta(T)
        for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr)
        {
            this->pstamp_ = max(this->pstamp_, (*itr).ver_->prev_->pstamp_);
        }

        // ssn_check_exclusion
        if (pstamp_ < sstamp_)
            this->status_ = TransactionStatus::committed;
        else
        {
            status_ = TransactionStatus::aborted;
            ++eres_->local_commitphase_counts_;
            SsnLock.unlock();
            return;
        }

        // update eta(V)
        for (auto itr = read_set_.begin(); itr != read_set_.end(); ++itr)
        {
            (*itr).ver_->pstamp_ = (max((*itr).ver_->pstamp_, this->cstamp_));
        }

        // update pi
        for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr)
        {
            (*itr).ver_->prev_->sstamp_ = this->sstamp_;
            // initialize new version
            (*itr).ver_->cstamp_ = (*itr).ver_->pstamp_ = this->cstamp_;
        }

        // status, inFlight -> committed ,w-unlock
        for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr)
        {
            (*itr).ver_->val_ = this->write_val_;
            (*itr).ver_->status_ = VersionStatus::committed;
            Tuple *tmp = get_tuple(itr->key_);
            tmp->lock_.w_unlock();
        }

        this->status_ = TransactionStatus::committed;

        // -------------------------------------------------------------------------------------
        if (use_lock == 1)
        {
            if (this->lock_flag == true) // r-unlock
            {
                this->lock_flag = false;
                for (auto itr = task_set_.begin(); itr != task_set_.end(); itr++)
                {
                    if (itr->ope_ == Ope::READ)
                    {
                        Tuple *tmp = get_tuple(itr->key_);
                        // tmp->lock_.r_unlock(this->sstamp_);
                        tmp->lock_.w_unlock();
                    }
                }
            }
        }
        // -------------------------------------------------------------------------------------
        SsnLock.unlock();
        read_set_.clear();
        write_set_.clear();
        return;
    }

    void abort()
    {
        for (auto itr = write_set_.begin(); itr != write_set_.end(); ++itr)
        {
            Version *next_committed = (*itr).ver_->prev_;
            while (next_committed->status_ != VersionStatus::committed)
                next_committed = next_committed->prev_;
            // cancel successor mark(sstamp).
            next_committed->sstamp_ = UINT32_MAX;
            (*itr).ver_->status_ = VersionStatus::aborted;
        }
        write_set_.clear();

        // notify that this transaction finishes reading the version now.
        read_set_.clear();
        ++eres_->local_abort_counts_;

        // -------------------------------------------------------------------------------------
        if (use_lock == 1 && isreadonly() == true)
        {
            // 提案手法: read only transactionのlock
            // cout << "all read but abort" << endl;
            for (auto itr = task_set_.begin(); itr != task_set_.end(); itr++)
            {
                if (itr->ope_ == Ope::READ)
                {
                    Tuple *tmp = get_tuple(itr->key_);
                    while (tmp->lock_.counter != 1)
                    {
                        tmp->lock_.w_getlock(this->pstamp_);
                    }
                }
            }
            this->lock_flag = true;
            ++eres_->local_uselock_counts_;
            return;
        }
        // -------------------------------------------------------------------------------------
    }

    void verify_exclusion_or_abort()
    {
        if (this->pstamp_ >= this->sstamp_)
        {
            this->status_ = TransactionStatus::aborted;
        }
    }

    static Tuple *get_tuple(uint64_t key)
    {
        return (&Table[key]);
    }

    bool isreadonly()
    {
        for (auto itr = task_set_.begin(); itr != task_set_.end(); itr++)
        {
            if (itr->ope_ == Ope::WRITE)
            {
                return false;
            }
        }
        return true;
    }
};

void displayParameter()
{
    cout << "max_ope:\t\t\t" << max_ope << endl;
    cout << "rratio:\t\t\t\t" << rratio << endl;
    cout << "thread_num:\t\t\t" << thread_num << endl;
}

void displayDB()
{
    Tuple *tuple;
    Version *version;

    for (int i = 0; i < tuple_num; ++i)
    {
        // for (auto itr = Table->begin(); itr != Table->end(); itr++) {
        tuple = &Table[i];
        cout << "------------------------------" << endl; // - 30
        cout << "key: " << i << endl;

        // version = tuple->latest_;
        version = tuple->latest_;

        while (version != NULL)
        {
            cout << "val: " << version->val_;

            switch (version->status_)
            {
            case VersionStatus::inFlight:
                cout << " status:  inFlight/";
                break;
            case VersionStatus::aborted:
                cout << " status:  aborted/";
                break;
            case VersionStatus::committed:
                cout << " status:  committed/";
                break;
            }
            // cout << endl;

            cout << " /cstamp:  " << version->cstamp_;
            cout << " /pstamp:  " << version->pstamp_;
            cout << " /sstamp:  " << version->sstamp_ << endl;
            // cout << endl;

            version = version->prev_;
        }
    }
}

void makeDB()
{
    posix_memalign((void **)&Table, PAGE_SIZE, (tuple_num) * sizeof(Tuple));
    for (int i = 0; i < tuple_num; i++)
    {
        Table[i].key = 0;
        Version *verTmp = new Version;
        verTmp->cstamp_ = 0;
        verTmp->pstamp_ = 0;
        verTmp->sstamp_ = UINT32_MAX;
        verTmp->prev_ = nullptr;
        verTmp->status_ = VersionStatus::committed;
        verTmp->val_ = 0;
        Table[i].latest_ = verTmp;
    }
}

void worker(size_t thid, char &ready, const bool &start, const bool &quit)
{
    Xoroshiro128Plus rnd;
    rnd.init();
    Result &myres = std::ref(ErmiaResult[thid]);
    FastZipf zipf(&rnd, 0, tuple_num);

    Transaction trans(thid, (Result *)&ErmiaResult[thid]);

    ready = true;

    while (start == false)
    {
    }

    while (quit == false)
    {
        makeTask(trans.task_set_, rnd, zipf);
        // viewtask(trans.task_set_);

    RETRY:
        if (quit == true)
            break;

        trans.tbegin();
        for (auto itr = trans.task_set_.begin(); itr != trans.task_set_.end();
             ++itr)
        {
            if ((*itr).ope_ == Ope::READ)
            {
                trans.ssn_tread((*itr).key_);
            }
            else if ((*itr).ope_ == Ope::WRITE)
            {
                trans.ssn_twrite((*itr).key_);
            }
            // early abort.
            if (trans.status_ == TransactionStatus::aborted)
            {
                trans.abort();

                goto RETRY;
            }
        }
        trans.ssn_commit();
        if (trans.status_ == TransactionStatus::committed)
        {
            myres.local_commit_counts_++;
        }
        else if (trans.status_ == TransactionStatus::aborted)
        {
            trans.abort();
            goto RETRY;
        }
    }
    return;
}

int main(int argc, char *argv[])
{
    displayParameter();
    makeDB();

    bool start = false;
    bool quit = false;
    initResult();
    std::vector<char> readys(thread_num);

    std::vector<std::thread> thv;
    for (size_t i = 0; i < thread_num; ++i)
    {
        thv.emplace_back(worker, i, std::ref(readys[i]), std::ref(start),
                         std::ref(quit));
    }
    waitForReady(readys);
    start = true;
    this_thread::sleep_for(std::chrono::milliseconds(1000 * extime));
    quit = true;

    for (auto &th : thv)
    {
        th.join();
    }

    for (unsigned int i = 0; i < thread_num; ++i)
    {
        ErmiaResult[0].addLocalAllResult(ErmiaResult[i]);
    }
    ErmiaResult[0].displayAllResult();

    // displayDB();

    return 0;
}