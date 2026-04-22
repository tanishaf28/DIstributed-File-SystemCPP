/**
 * ============================================================
 *  GENERIC DATA PROCESSING FRAMEWORK — Mini Spark Engine
 *  C++17 Reference Implementation
 * ============================================================
 *
 * BUILD:
 *   g++ -std=c++17 -pthread -O2 -o data_pipeline data_pipeline.cpp
 *
 * RUN:
 *   ./data_pipeline
 *
 * ============================================================
 *  CONCEPTS COVERED
 * ============================================================
 *  [TEMPLATE]     - Dataset<T> generic container
 *  [LAMBDA]       - map/filter/reduce operations
 *  [CONCEPT]      - C++20-style type constraints (via SFINAE)
 *  [VARIADIC]     - Variadic template pipeline composition
 *  [MOVE]         - Move semantics for pipeline chaining
 *  [STL]          - vector, algorithm, numeric, functional
 *  [THREAD]       - Optional parallel execution
 *  [SMART_PTR]    - unique_ptr for pipeline stages
 *  [FUNCTIONAL]   - std::function, composition
 *  [ITERATOR]     - Custom iterator on Dataset<T>
 * ============================================================
 */

#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <algorithm>
#include <numeric>
#include <optional>
#include <map>
#include <unordered_map>
#include <memory>
#include <thread>
#include <mutex>
#include <future>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <cassert>
#include <type_traits>
#include <iterator>
#include <chrono>

// ══════════════════════════════════════════════════════════════
//  SECTION 1: DATASET<T> — GENERIC LAZY CONTAINER [TEMPLATE]
// ══════════════════════════════════════════════════════════════

/**
 * [TEMPLATE] Dataset<T> — the core data container.
 * Wraps a std::vector<T> and exposes a fluent pipeline API.
 * All transformations return a NEW Dataset (immutable style).
 *
 * Demonstrates:
 *  - Class templates
 *  - Template member functions with different return types
 *  - Method chaining (builder pattern)
 *  - [MOVE] Efficient chaining via move semantics
 */
template<typename T>
class Dataset {
    std::vector<T> data_;

public:
    // ─── Constructors ─────────────────────────────────────

    Dataset() = default;

    explicit Dataset(std::vector<T> data)
        : data_(std::move(data)) {}       // [MOVE]

    // Range constructor: Dataset from [begin, end)
    template<typename Iter>
    Dataset(Iter begin, Iter end)
        : data_(begin, end) {}

    // Initializer list: Dataset<int> d = {1,2,3,4,5};
    Dataset(std::initializer_list<T> il)
        : data_(il) {}

    // ─── Accessors ────────────────────────────────────────

    const std::vector<T>& data() const { return data_; }
    size_t size()  const { return data_.size(); }
    bool   empty() const { return data_.empty(); }

    // [ITERATOR] Forward iterators
    auto begin() const { return data_.begin(); }
    auto end()   const { return data_.end(); }
    auto begin()       { return data_.begin(); }
    auto end()         { return data_.end(); }

    // ─── TRANSFORMATIONS ──────────────────────────────────

    /**
     * [LAMBDA] [TEMPLATE] map<U>(f) — transform each element T → U.
     * Returns Dataset<U>, enabling chains like:
     *   ds.map([](int x){ return x*2; }).map([](int x){ return std::to_string(x); })
     */
    template<typename F,
             typename U = std::invoke_result_t<F, T>>
    Dataset<U> map(F&& f) const {
        std::vector<U> result;
        result.reserve(data_.size());
        std::transform(data_.begin(), data_.end(),
                       std::back_inserter(result),
                       std::forward<F>(f));           // perfect forwarding
        return Dataset<U>(std::move(result));         // [MOVE]
    }

    /**
     * [LAMBDA] filter(predicate) — keep elements where pred(x) == true.
     * Returns Dataset<T>.
     */
    template<typename Pred>
    Dataset<T> filter(Pred&& pred) const {
        std::vector<T> result;
        std::copy_if(data_.begin(), data_.end(),
                     std::back_inserter(result),
                     std::forward<Pred>(pred));
        return Dataset<T>(std::move(result));         // [MOVE]
    }

    /**
     * [LAMBDA] reduce(init, f) — fold elements into a single value.
     * f: (Acc, T) → Acc
     */
    template<typename Acc, typename F>
    Acc reduce(Acc init, F&& f) const {
        return std::accumulate(data_.begin(), data_.end(),
                               std::move(init),       // [MOVE]
                               std::forward<F>(f));
    }

    /**
     * forEach — apply f to every element (side effects).
     */
    template<typename F>
    const Dataset<T>& forEach(F&& f) const {
        std::for_each(data_.begin(), data_.end(), std::forward<F>(f));
        return *this;
    }

    /**
     * flatMap — map then flatten.
     * f: T → vector<U>  ⟶  Dataset<U>
     */
    template<typename F,
             typename Inner = typename std::invoke_result_t<F,T>::value_type>
    Dataset<Inner> flatMap(F&& f) const {
        std::vector<Inner> result;
        for (const auto& item : data_) {
            auto sub = f(item);
            result.insert(result.end(),
                          std::make_move_iterator(sub.begin()),   // [MOVE]
                          std::make_move_iterator(sub.end()));
        }
        return Dataset<Inner>(std::move(result));
    }

    /**
     * take(n) — first n elements.
     */
    Dataset<T> take(size_t n) const {
        size_t count = std::min(n, data_.size());
        return Dataset<T>(std::vector<T>(data_.begin(), data_.begin() + count));
    }

    /**
     * drop(n) — skip first n elements.
     */
    Dataset<T> drop(size_t n) const {
        if (n >= data_.size()) return Dataset<T>{};
        return Dataset<T>(std::vector<T>(data_.begin() + n, data_.end()));
    }

    /**
     * sort — returns sorted copy.
     */
    template<typename Comp = std::less<T>>
    Dataset<T> sorted(Comp comp = {}) const {
        std::vector<T> copy = data_;
        std::sort(copy.begin(), copy.end(), comp);
        return Dataset<T>(std::move(copy));
    }

    /**
     * distinct — remove duplicates (order preserved).
     */
    Dataset<T> distinct() const {
        std::vector<T> result;
        for (const auto& item : data_) {
            if (std::find(result.begin(), result.end(), item) == result.end())
                result.push_back(item);
        }
        return Dataset<T>(std::move(result));
    }

    /**
     * [TEMPLATE] groupBy<K>(key_fn) — group into map<K, Dataset<T>>.
     * Example: group words by first letter.
     */
    template<typename F,
             typename K = std::invoke_result_t<F, T>>
    std::map<K, Dataset<T>> groupBy(F&& key_fn) const {
        std::map<K, std::vector<T>> groups;
        for (const auto& item : data_)
            groups[key_fn(item)].push_back(item);

        std::map<K, Dataset<T>> result;
        for (auto& [k, v] : groups)
            result.emplace(k, Dataset<T>(std::move(v)));  // [MOVE]
        return result;
    }

    /**
     * zip — combine two datasets element-wise.
     * Returns Dataset of pairs, length = min(this, other).
     */
    template<typename U>
    Dataset<std::pair<T,U>> zip(const Dataset<U>& other) const {
        size_t n = std::min(data_.size(), other.size());
        std::vector<std::pair<T,U>> result;
        result.reserve(n);
        for (size_t i = 0; i < n; ++i)
            result.emplace_back(data_[i], other.data()[i]);
        return Dataset<std::pair<T,U>>(std::move(result));
    }

    // ─── AGGREGATES ───────────────────────────────────────

    size_t count() const { return data_.size(); }

    // Numeric sum (requires T to be addable)
    T sum() const {
        return std::accumulate(data_.begin(), data_.end(), T{});
    }

    std::optional<T> min() const {
        if (data_.empty()) return std::nullopt;
        return *std::min_element(data_.begin(), data_.end());
    }

    std::optional<T> max() const {
        if (data_.empty()) return std::nullopt;
        return *std::max_element(data_.begin(), data_.end());
    }

    // ─── PARALLEL EXECUTION [THREAD] ──────────────────────

    /**
     * [THREAD] parallelMap — splits data into chunks and runs
     * map() on each chunk in a separate thread.
     * Demonstrates std::future + std::async.
     */
    template<typename F,
             typename U = std::invoke_result_t<F, T>>
    Dataset<U> parallelMap(F f, size_t n_threads = 4) const {
        size_t chunk = (data_.size() + n_threads - 1) / n_threads;
        std::vector<std::future<std::vector<U>>> futures;

        for (size_t t = 0; t < n_threads; ++t) {
            size_t start = t * chunk;
            size_t end   = std::min(start + chunk, data_.size());
            if (start >= data_.size()) break;

            // [THREAD] Launch async task for this chunk
            futures.push_back(std::async(std::launch::async,
                [&, start, end]() {
                    std::vector<U> part;
                    part.reserve(end - start);
                    for (size_t i = start; i < end; ++i)
                        part.push_back(f(data_[i]));
                    return part;
                }
            ));
        }

        // Collect results
        std::vector<U> result;
        for (auto& fut : futures) {
            auto part = fut.get();
            result.insert(result.end(),
                          std::make_move_iterator(part.begin()),  // [MOVE]
                          std::make_move_iterator(part.end()));
        }
        return Dataset<U>(std::move(result));
    }

    // ─── IO ───────────────────────────────────────────────

    void print(const std::string& label = "") const {
        if (!label.empty()) std::cout << label << ": ";
        std::cout << "[";
        for (size_t i = 0; i < data_.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << data_[i];
        }
        std::cout << "] (size=" << data_.size() << ")\n";
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 2: PIPELINE COMPOSITION [VARIADIC] [FUNCTIONAL]
// ══════════════════════════════════════════════════════════════

/**
 * [VARIADIC] compose(f, g, h, ...) — compose N functions left-to-right.
 * compose(f, g)(x) == g(f(x))
 *
 * Uses variadic templates and fold expressions (C++17).
 */
template<typename F>
auto compose(F&& f) {
    return std::forward<F>(f);
}

template<typename F, typename G, typename... Rest>
auto compose(F&& f, G&& g, Rest&&... rest) {
    return compose(
        [f = std::forward<F>(f), g = std::forward<G>(g)](auto&& x) {
            return g(f(std::forward<decltype(x)>(x)));
        },
        std::forward<Rest>(rest)...
    );
}

/**
 * [SMART_PTR] [POLYMORPHISM] PipelineStage — abstract stage.
 * Enables building named, reusable pipeline steps.
 */
template<typename In, typename Out>
class PipelineStage {
public:
    virtual ~PipelineStage() = default;
    virtual Dataset<Out> process(const Dataset<In>& input) const = 0;
    virtual std::string  name() const = 0;
};

/**
 * LambdaStage — wraps a lambda as a pipeline stage.
 */
template<typename In, typename Out>
class LambdaStage : public PipelineStage<In, Out> {
    std::string name_;
    std::function<Dataset<Out>(const Dataset<In>&)> fn_;
public:
    LambdaStage(std::string name,
                std::function<Dataset<Out>(const Dataset<In>&)> fn)
        : name_(std::move(name)), fn_(std::move(fn)) {}

    Dataset<Out> process(const Dataset<In>& input) const override {
        return fn_(input);
    }
    std::string name() const override { return name_; }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 3: WORD COUNT DEMO — classic MapReduce
// ══════════════════════════════════════════════════════════════

/**
 * Demonstrates: flatMap, groupBy, map, sorted, [LAMBDA] throughout.
 * Input: vector of sentences → word frequency map.
 */
void wordCountDemo() {
    std::cout << "\n=== WORD COUNT (MapReduce style) ===\n";

    Dataset<std::string> sentences = {
        "the quick brown fox",
        "the fox jumped over the lazy dog",
        "the dog is quick"
    };

    // Step 1: flatMap sentences → words
    auto words = sentences.flatMap([](const std::string& s) {
        std::vector<std::string> ws;
        std::istringstream ss(s);
        std::string w;
        while (ss >> w) ws.push_back(w);
        return ws;
    });

    words.print("All words");

    // Step 2: groupBy word → Dataset of occurrences
    auto groups = words.groupBy([](const std::string& w){ return w; });

    // Step 3: count each group → word frequency
    std::vector<std::pair<std::string,int>> freq;
    for (const auto& [word, ds] : groups)
        freq.emplace_back(word, static_cast<int>(ds.count()));

    // Step 4: sort by frequency descending
    std::sort(freq.begin(), freq.end(),
              [](const auto& a, const auto& b){ return a.second > b.second; });

    std::cout << "Word frequencies:\n";
    for (const auto& [w, c] : freq)
        std::cout << "  " << w << ": " << c << "\n";
}


// ══════════════════════════════════════════════════════════════
//  SECTION 4: NUMERIC PIPELINE DEMO
// ══════════════════════════════════════════════════════════════

void numericPipelineDemo() {
    std::cout << "\n=== NUMERIC PIPELINE ===\n";

    // Generate 1..20
    std::vector<int> raw(20);
    std::iota(raw.begin(), raw.end(), 1);
    Dataset<int> numbers(std::move(raw));  // [MOVE]

    numbers.print("Input");

    // Pipeline: keep evens → multiply by 3 → drop first 2 → take 5
    auto result = numbers
        .filter([](int x){ return x % 2 == 0; })
        .map([](int x){ return x * 3; })
        .drop(2)
        .take(5);

    result.print("After filter(even) → map(*3) → drop(2) → take(5)");

    // Aggregate
    std::cout << "Sum: "  << result.sum() << "\n";
    std::cout << "Min: "  << *result.min() << "\n";
    std::cout << "Max: "  << *result.max() << "\n";
    std::cout << "Count: "<< result.count() << "\n";

    // Zip two datasets
    Dataset<std::string> labels = {"a","b","c","d","e"};
    auto zipped = result.zip(labels);
    std::cout << "Zipped:\n";
    zipped.forEach([](const auto& p){
        std::cout << "  (" << p.first << ", " << p.second << ")\n";
    });
}


// ══════════════════════════════════════════════════════════════
//  SECTION 5: PARALLEL MAP DEMO [THREAD]
// ══════════════════════════════════════════════════════════════

void parallelDemo() {
    std::cout << "\n=== PARALLEL MAP ===\n";

    // 1 million integers
    std::vector<long long> raw(1'000'000);
    std::iota(raw.begin(), raw.end(), 1LL);
    Dataset<long long> big(std::move(raw));  // [MOVE]

    auto t0 = std::chrono::high_resolution_clock::now();

    // Parallel: square each element across 4 threads
    auto squared = big.parallelMap([](long long x){ return x * x; }, 4);

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double,std::milli>(t1-t0).count();

    std::cout << "parallelMap on " << big.size() << " elements: "
              << ms << " ms\n";
    std::cout << "First 5 squared: ";
    squared.take(5).print();
}


// ══════════════════════════════════════════════════════════════
//  SECTION 6: COMPOSE & NAMED PIPELINE DEMO [VARIADIC]
// ══════════════════════════════════════════════════════════════

void composeDemo() {
    std::cout << "\n=== FUNCTION COMPOSITION ===\n";

    // [VARIADIC] Compose three lambdas into one
    auto pipeline = compose(
        [](int x){ return x + 1; },     // step 1: increment
        [](int x){ return x * 2; },     // step 2: double
        [](int x){ return x - 3; }      // step 3: subtract 3
    );

    // Apply to a dataset
    Dataset<int> ds = {1, 2, 3, 4, 5};
    auto result = ds.map(pipeline);
    result.print("compose(+1, *2, -3)");

    // Named pipeline stages with PipelineStage [SMART_PTR]
    using Stage = std::unique_ptr<PipelineStage<int,int>>;
    std::vector<Stage> stages;

    stages.push_back(std::make_unique<LambdaStage<int,int>>(
        "FilterOdd",
        [](const Dataset<int>& d){ return d.filter([](int x){ return x%2!=0; }); }
    ));
    stages.push_back(std::make_unique<LambdaStage<int,int>>(
        "Triple",
        [](const Dataset<int>& d){ return d.map([](int x){ return x*3; }); }
    ));

    Dataset<int> data = {1,2,3,4,5,6,7,8,9,10};
    for (auto& stage : stages) {
        std::cout << "Running stage: " << stage->name() << "\n";
        data = stage->process(data);
    }
    data.print("After named pipeline");
}


// ══════════════════════════════════════════════════════════════
//  SECTION 7: CSV LOADER — real-world usage [STL]
// ══════════════════════════════════════════════════════════════

struct Record {
    std::string name;
    int         score;
    std::string grade;

    friend std::ostream& operator<<(std::ostream& os, const Record& r) {
        return os << r.name << "(" << r.score << "," << r.grade << ")";
    }
};

void csvDemo() {
    std::cout << "\n=== CSV PROCESSING DEMO ===\n";

    // Simulate CSV data
    std::vector<Record> raw = {
        {"Alice", 92, "A"}, {"Bob", 74, "C"}, {"Carol", 88, "B"},
        {"Dave",  55, "F"}, {"Eve",  96, "A"}, {"Frank", 81, "B"},
        {"Grace", 60, "D"}, {"Hank", 91, "A"}
    };
    Dataset<Record> records(std::move(raw));

    // Filter A-grade students, sorted by score desc
    auto top = records
        .filter([](const Record& r){ return r.grade == "A"; })
        .sorted([](const Record& a, const Record& b){ return a.score > b.score; });

    std::cout << "Top students (grade A):\n";
    top.forEach([](const Record& r){
        std::cout << "  " << r.name << ": " << r.score << "\n";
    });

    // Average score of all students
    double avg = static_cast<double>(
        records.map([](const Record& r){ return r.score; }).sum()
    ) / records.count();
    std::cout << "Class average: " << avg << "\n";

    // Group by grade
    auto groups = records.groupBy([](const Record& r){ return r.grade; });
    std::cout << "Students per grade:\n";
    for (const auto& [grade, ds] : groups)
        std::cout << "  " << grade << ": " << ds.count() << "\n";
}


// ══════════════════════════════════════════════════════════════
//  SECTION 8: MAIN
// ══════════════════════════════════════════════════════════════

// Runtime flow:
// 1. `main()` prints a banner and runs the demo functions in sequence.
// 2. Each demo builds a Dataset<T>, applies transformations, and prints the
//    result so the pipeline behavior is visible from the terminal.
// 3. The parallel demo uses `std::async` to show the same Dataset API running
//    across multiple worker tasks without changing the calling code.

int main() {
    std::cout << "=== Generic Data Processing Framework ===\n";
    numericPipelineDemo();
    wordCountDemo();
    parallelDemo();
    composeDemo();
    csvDemo();
    std::cout << "\n=== All demos complete ===\n";
    return 0;
}

/*
 * ============================================================
 *  EXTENSION IDEAS
 * ============================================================
 *  1. Lazy evaluation — store transformations as a graph,
 *     only execute on collect() / terminal operation
 *  2. Disk-spill — when dataset exceeds RAM, write to tmp file
 *  3. SQL-like DSL — SELECT, WHERE, GROUP BY, ORDER BY wrappers
 *  4. Schema inference — auto-detect CSV column types
 *  5. Distributed partitions — send partitions over TCP and
 *     collect results (mini distributed Spark)
 * ============================================================
 */
