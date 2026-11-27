// Standalone host diagnostic: cl /nologo /std:c++20 /EHsc /O2 m04_tree.cpp
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Key = std::uint32_t;
constexpr Key none = std::numeric_limits<Key>::max();
constexpr std::size_t pageBytes = 4096;
using Page = std::array<char, pageBytes>;
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

struct Box { double x0, y0, x1, y1; };
struct Record { Box box; std::int64_t id; };
static_assert(sizeof(Record) == 40);
constexpr Box exportView{112, 112, 368, 368};

bool overlaps(Box a, Box b) {
    return a.x0 <= b.x1 && a.x1 >= b.x0 && a.y0 <= b.y1 && a.y1 >= b.y0;
}
Box cover(Box a, Box b) {
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0),
            std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}
bool valid(Box b) {
    return std::isfinite(b.x0) && std::isfinite(b.y0) && std::isfinite(b.x1) &&
           std::isfinite(b.y1) && b.x0 <= b.x1 && b.y0 <= b.y1;
}
bool oracleIntersects(Box b, Box q) {
    return !(b.x1 < q.x0 || q.x1 < b.x0 || b.y1 < q.y0 || q.y1 < b.y0);
}
double micros(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
template<class T> T get(const Page& p, std::size_t offset) {
    T v;
    std::memcpy(&v, p.data() + offset, sizeof v);
    return v;
}
template<class T> void put(Page& p, std::size_t offset, const T& v) {
    std::memcpy(p.data() + offset, &v, sizeof v);
}
template<class T> std::size_t bytes(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

std::vector<Record> readInput(const fs::path& path) {
    const auto size = fs::file_size(path);
    if (size == 0 || size % sizeof(Record) || size / sizeof(Record) > none / 2)
        throw std::runtime_error("unsupported input size (expected nonempty 40-byte records)");
    std::ifstream f(path, std::ios::binary);
    std::vector<Record> records(static_cast<std::size_t>(size / sizeof(Record)));
    for (auto& r : records) {
        if (!f.read(reinterpret_cast<char*>(&r), sizeof r) || !valid(r.box))
            throw std::runtime_error("short input read or invalid bounds");
    }
    if (f.peek() != std::char_traits<char>::eof() || f.bad())
        throw std::runtime_error("input size changed during read");
    return records;
}

// The files are disposable native little-endian benchmark artifacts, not sidecars.
class File {
    std::fstream io_;
    fs::path path_;
public:
    Key pages = 0;
    explicit File(const fs::path& path) : path_(path) {
        io_.rdbuf()->pubsetbuf(nullptr, 0);
        io_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!io_) throw std::runtime_error("cannot create tree file");
    }
    void append(const Page& p) {
        if (!io_.write(p.data(), p.size())) throw std::runtime_error("tree append failed");
        ++pages;
    }
    void read(Key page, Page& p) {
        if (page >= pages) throw std::runtime_error("invalid page reference");
        io_.seekg(static_cast<std::streamoff>(page) * pageBytes);
        if (!io_.read(p.data(), p.size())) throw std::runtime_error("tree read failed");
    }
    void overwrite(Key page, const Page& p) {
        if (page >= pages) throw std::runtime_error("invalid page write");
        io_.seekp(static_cast<std::streamoff>(page) * pageBytes);
        if (!io_.write(p.data(), p.size())) throw std::runtime_error("tree update failed");
    }
    void flush() {
        if (!io_.flush()) throw std::runtime_error("tree flush failed");
    }
    std::uintmax_t size() const {
        const auto actual = fs::file_size(path_);
        if (actual != std::uintmax_t{pages} * pageBytes)
            throw std::runtime_error("unexpected tree file size");
        return actual;
    }
};

struct Work {
    std::uint64_t nodes = 0, internalTests = 0, spanTests = 0, refs = 0, reads = 0;
};
class Cache {
    static constexpr std::size_t sets = 64, ways = 4;
    struct Slot { Page data; Key page = none; std::uint64_t used = 0; };
    std::vector<Slot> slots_ = std::vector<Slot>(sets * ways);
    File* owner_ = nullptr;
    std::uint64_t clock_ = 0;
public:
    void reset(File& f) {
        for (auto& s : slots_) { s.page = none; s.used = 0; }
        clock_ = 0;
        owner_ = &f;
    }
    const Page& fetch(Key page, Work& w) {
        ++w.refs;
        auto* first = &slots_[(page % sets) * ways];
        auto* victim = first;
        for (auto* s = first; s != first + ways; ++s) {
            if (s->page == page) { s->used = ++clock_; return s->data; }
            if (s->used < victim->used) victim = s;
        }
        owner_->read(page, victim->data);
        ++w.reads;
        victim->page = page;
        victim->used = ++clock_;
        return victim->data;
    }
    std::size_t capacityBytes() const { return bytes(slots_) + sizeof(*this); }
    static constexpr std::size_t payloadBytes = sets * ways * pageBytes;
};

// Packed: one logical node per page. STR sorting is repeated at each level.
struct Entry { Box box; std::int64_t id; Key key; Key reserved = 0; };
struct Header { Key count, leaf; };
constexpr std::size_t fanout = (pageBytes - sizeof(Header)) / sizeof(Entry);
static_assert(sizeof(Entry) == 48 && fanout == 85);
std::size_t entryOffset(Key i) { return sizeof(Header) + i * sizeof(Entry); }
Header header(const Page& p) {
    const auto h = get<Header>(p, 0);
    if (h.count == 0 || h.count > fanout || h.leaf > 1)
        throw std::runtime_error("invalid packed node");
    return h;
}
void strSort(std::vector<Entry>& level) {
    auto by = [](bool y) {
        return [y](const Entry& a, const Entry& b) {
            auto center = [y](Box s) {
                return y ? std::midpoint(s.y0, s.y1) : std::midpoint(s.x0, s.x1);
            };
            const auto x = center(a.box), z = center(b.box);
            return x == z ? a.key < b.key : x < z;
        };
    };
    const auto pages = (level.size() + fanout - 1) / fanout;
    const auto slices = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(pages))));
    const auto stripe = ((pages + slices - 1) / slices) * fanout;
    std::sort(level.begin(), level.end(), by(false));
    for (std::size_t i = 0; i < level.size(); i += stripe)
        std::sort(level.begin() + i, level.begin() + std::min(i + stripe, level.size()), by(true));
}

// Dynamic: insert each record in incoming order, using greedy perimeter growth
// and local AVL-style rotations. No centroid sort or post-build reorder.
struct Node { Box box; std::int64_t id; Key left, right, rank, height; };
static_assert(sizeof(Node) == 56);
constexpr std::size_t nodesPerPage = pageBytes / sizeof(Node);
Key nodePage(Key key) { return static_cast<Key>(key / nodesPerPage); }
std::size_t nodeOffset(Key key) { return (key % nodesPerPage) * sizeof(Node); }

struct DynamicBuild {
    std::vector<Node> nodes;
    Key append(Node n) {
        nodes.push_back(n);
        return static_cast<Key>(nodes.size() - 1);
    }
    void refit(Key k) {
        auto& n = nodes[k];
        const auto& a = nodes[n.left];
        const auto& b = nodes[n.right];
        n.box = cover(a.box, b.box);
        n.height = 1 + std::max(a.height, b.height);
    }
    Key rotateLeft(Key k) {
        const auto r = nodes[k].right;
        nodes[k].right = nodes[r].left;
        nodes[r].left = k;
        refit(k);
        refit(r);
        return r;
    }
    Key rotateRight(Key k) {
        const auto l = nodes[k].left;
        nodes[k].left = nodes[l].right;
        nodes[l].right = k;
        refit(k);
        refit(l);
        return l;
    }
    Key balance(Key k) {
        const auto l = nodes[k].left, r = nodes[k].right;
        if (nodes[l].height > nodes[r].height + 1) {
            if (nodes[nodes[l].right].height > nodes[nodes[l].left].height)
                nodes[k].left = rotateLeft(l);
            return rotateRight(k);
        }
        if (nodes[r].height > nodes[l].height + 1) {
            if (nodes[nodes[r].left].height > nodes[nodes[r].right].height)
                nodes[k].right = rotateRight(r);
            return rotateLeft(k);
        }
        refit(k);
        return k;
    }
    static double growth(Box b, Box p) {
        // Halve before subtraction so finite input endpoints cannot overflow
        // in each difference. Compare half-projections to avoid inf-inf.
        return ((std::max(b.x1, p.x1) * .25 - b.x1 * .25) +
                (b.x0 * .25 - std::min(b.x0, p.x0) * .25)) +
               ((std::max(b.y1, p.y1) * .25 - b.y1 * .25) +
                (b.y0 * .25 - std::min(b.y0, p.y0) * .25));
    }
    Key insert(Key root, Key leaf) {
        if (root == none) return leaf;
        const auto l = nodes[root].left, r = nodes[root].right;
        if (l == none) return append({cover(nodes[root].box, nodes[leaf].box), 0, root, leaf, none, 1});
        const auto a = growth(nodes[l].box, nodes[leaf].box);
        const auto b = growth(nodes[r].box, nodes[leaf].box);
        if (a < b || (a == b && nodes[l].height <= nodes[r].height)) {
            const auto next = insert(l, leaf);
            nodes[root].left = next;
        } else {
            const auto next = insert(r, leaf);
            nodes[root].right = next;
        }
        return balance(root);
    }
};

struct Hit {
    Key rank;
    std::int64_t id;
    bool operator==(const Hit&) const = default;
};

struct Tree {
    File file;
    bool packed;
    Key root = none;
    std::size_t depth = 0, logicalNodes = 0, buildVectorPeak = 0;
    double buildUs = 0;
    Tree(const fs::path& path, bool pack, const std::vector<Record>& records)
        : file(path), packed(pack) {
        const auto t = Clock::now();
        if (packed) {
            std::vector<Entry> level;
            level.reserve(records.size());
            for (Key k = 0; k < records.size(); ++k) level.push_back({records[k].box, records[k].id, k});
            for (;;) {
                ++depth;
                const auto n = (level.size() + fanout - 1) / fanout;
                std::vector<Entry> next;
                if (n > 1) next.reserve(n);
                buildVectorPeak = std::max(buildVectorPeak, bytes(level) + bytes(next));
                strSort(level);
                for (std::size_t i = 0; i < level.size(); i += fanout) {
                    const auto count = static_cast<Key>(std::min(fanout, level.size() - i));
                    Page p{};
                    put(p, 0, Header{count, depth == 1 ? 1u : 0u});
                    Box b = level[i].box;
                    for (Key j = 0; j < count; ++j) {
                        put(p, entryOffset(j), level[i + j]);
                        b = cover(b, level[i + j].box);
                    }
                    root = file.pages;
                    file.append(p);
                    if (n > 1) next.push_back({b, 0, root});
                }
                if (n == 1) break;
                level.swap(next);
            }
            logicalNodes = file.pages;
        } else {
            DynamicBuild build;
            build.nodes.reserve(2 * records.size() - 1);
            for (Key k = 0; k < records.size(); ++k) {
                const auto leaf = build.append({records[k].box, records[k].id, none, none, k, 0});
                root = build.insert(root, leaf);
            }
            depth = 1 + build.nodes[root].height;
            logicalNodes = build.nodes.size();
            buildVectorPeak = bytes(build.nodes);
            for (std::size_t i = 0; i < build.nodes.size(); i += nodesPerPage) {
                Page p{};
                const auto n = std::min(nodesPerPage, build.nodes.size() - i);
                std::memcpy(p.data(), build.nodes.data() + i, n * sizeof(Node));
                file.append(p);
            }
        }
        file.flush();
        buildUs = micros(t);
    }
    const char* name() const { return packed ? "packed_str" : "dynamic_aabb"; }
    std::size_t frontierBound() const { return 1 + (packed ? fanout - 1 : 1) * (depth - 1); }
    Work query(Cache& cache, Box view, std::vector<Key>& pending, std::vector<Hit>& hits) const {
        Work w;
        pending.clear();
        hits.clear();
        pending.push_back(root);
        while (!pending.empty()) {
            const auto key = pending.back();
            pending.pop_back();
            ++w.nodes;
            if (packed) {
                const auto& p = cache.fetch(key, w);
                const auto h = header(p);
                for (Key i = 0; i < h.count; ++i) {
                    const auto e = get<Entry>(p, entryOffset(i));
                    if (h.leaf) {
                        ++w.spanTests;
                        if (overlaps(e.box, view)) hits.push_back({e.key, e.id});
                    } else {
                        ++w.internalTests;
                        if (overlaps(e.box, view)) pending.push_back(e.key);
                    }
                }
            } else {
                const auto n = get<Node>(cache.fetch(nodePage(key), w), nodeOffset(key));
                if (n.left == none) {
                    ++w.spanTests;
                    if (overlaps(n.box, view)) hits.push_back({n.rank, n.id});
                } else {
                    ++w.internalTests;
                    if (overlaps(n.box, view)) {
                        pending.push_back(n.right);
                        pending.push_back(n.left);
                    }
                }
            }
        }
        return w;
    }
    // Spatially locate the SAME span for both trees, with no build-time locator.
    // Keep the ancestor path, then perform a refit without moving any entries.
    struct Step { Key key, slot; };
    bool find(Key k, Key rank, Box old, Cache& c, Work& w, std::vector<Step>& path) const {
        ++w.nodes;
        if (packed) {
            const Page p = c.fetch(k, w); // recursive fetches may evict this page
            const auto h = header(p);
            for (Key i = 0; i < h.count; ++i) {
                const auto e = get<Entry>(p, entryOffset(i));
                if (h.leaf) {
                    ++w.spanTests;
                    if (e.key == rank) { path.push_back({k, i}); return true; }
                } else {
                    ++w.internalTests;
                    if (!overlaps(e.box, old)) continue;
                    path.push_back({k, i});
                    if (find(e.key, rank, old, c, w, path)) return true;
                    path.pop_back();
                }
            }
            return false;
        }
        const auto n = get<Node>(c.fetch(nodePage(k), w), nodeOffset(k));
        if (n.left == none) {
            ++w.spanTests;
            if (n.rank != rank) return false;
            path.push_back({k, 0});
            return true;
        }
        ++w.internalTests;
        if (!overlaps(n.box, old)) return false;
        path.push_back({k, 0});
        if (find(n.left, rank, old, c, w, path) || find(n.right, rank, old, c, w, path))
            return true;
        path.pop_back();
        return false;
    }
    void edit(Cache& cache, Key rank, Box old, Box moved) {
        struct Dirty { Key key; Page page; };
        std::vector<Step> path;
        std::vector<Dirty> dirty;
        path.reserve(depth);
        dirty.reserve(depth);
        Work lookup, refit;
        cache.reset(file);
        const auto t = Clock::now();
        if (!find(root, rank, old, cache, lookup, path))
            throw std::runtime_error("edit target missing");
        auto findDirty = [&](Key key) {
            return std::find_if(dirty.begin(), dirty.end(), [&](const auto& p) { return p.key == key; });
        };
        auto read = [&](Key key) -> const Page& {
            const auto i = findDirty(key);
            return i == dirty.end() ? cache.fetch(key, refit) : i->page;
        };
        auto write = [&](Key key) -> Page& {
            const auto i = findDirty(key);
            if (i != dirty.end()) return i->page;
            dirty.push_back({key, cache.fetch(key, refit)});
            return dirty.back().page;
        };
        Box updated = moved;
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            if (packed) {
                auto& p = write(it->key);
                auto e = get<Entry>(p, entryOffset(it->slot));
                e.box = updated;
                put(p, entryOffset(it->slot), e);
                const auto h = header(p);
                updated = get<Entry>(p, entryOffset(0)).box;
                for (Key i = 1; i < h.count; ++i)
                    updated = cover(updated, get<Entry>(p, entryOffset(i)).box);
            } else {
                auto node = [&](Key k) { return get<Node>(read(nodePage(k)), nodeOffset(k)); };
                auto n = node(it->key);
                if (n.left == none) n.box = moved;
                else {
                    const auto l = node(n.left);
                    const auto r = node(n.right);
                    n.box = cover(l.box, r.box);
                }
                put(write(nodePage(it->key)), nodeOffset(it->key), n);
            }
        }
        for (const auto& p : dirty) file.overwrite(p.key, p.page);
        file.flush();
        const auto us = micros(t);
        cache.reset(file);
        std::cout << "edit tree=" << name() << " us=" << us << " path_nodes=" << path.size()
                  << " lookup_nodes=" << lookup.nodes << " lookup_page_refs=" << lookup.refs
                  << " lookup_reads=" << lookup.reads << " refit_page_refs=" << refit.refs
                  << " refit_reads=" << refit.reads << " read_bytes=" << (lookup.reads + refit.reads) * pageBytes
                  << " pages_written=" << dirty.size() << " bytes_written=" << dirty.size() * pageBytes
                  << " write_amp_vs_32B_bounds=" << dirty.size() * pageBytes / sizeof(Box)
                  << " edit_vector_capacity_B=" << bytes(path) + bytes(dirty)
                  << " lookup_page_copy_stack_bound_B=" << (packed ? depth * pageBytes : 0) << '\n';
    }
};

void linear(const std::vector<Record>& records, Box view, std::vector<Hit>& hits) {
    hits.clear();
    for (Key k = 0; k < records.size(); ++k)
        if (oracleIntersects(records[k].box, view)) hits.push_back({k, records[k].id});
}
void check(std::vector<Hit>& actual, const std::vector<Hit>& expected) {
    std::sort(actual.begin(), actual.end(), [](Hit a, Hit b) { return a.rank < b.rank; });
    if (actual != expected) throw std::runtime_error("tree/linear oracle span or ID mismatch");
}
std::vector<std::int64_t> uniqueIds(const std::vector<Hit>& hits) {
    std::vector<std::int64_t> ids;
    ids.reserve(hits.size());
    for (const auto& h : hits) ids.push_back(h.id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::size_t strokeRuns(const std::vector<Record>& records) {
    std::size_t count = 1;
    for (std::size_t k = 1; k < records.size(); ++k)
        if (records[k - 1].id != records[k].id) ++count;
    return count;
}
Box atBlock(Box b, std::size_t block) {
    const auto x = 1024.0 * static_cast<double>(block);
    return {b.x0 + x, b.y0, b.x1 + x, b.y1};
}
struct Workload { const char* name; std::vector<Box> views; };
std::vector<Workload> workloads(const std::vector<Record>& records, std::size_t runs) {
    const auto blocks = std::max(std::size_t{1}, runs / 256);
    std::vector<Workload> groups{{"sparse_8x8", {}}, {"boundary", {}}, {"full_block", {}}};
    for (std::size_t i = 0; i < 128; ++i)
        groups[0].views.push_back(atBlock(exportView, i * (blocks - 1) / 127));
    for (std::size_t i = 0; i < 16; ++i) {
        const auto block = i * (blocks - 1) / 15;
        const auto& b = records[i * (records.size() - 1) / 15].box;
        groups[1].views.push_back({b.x0, b.y0, b.x0, b.y0}); // inclusive contact with actual input
        groups[1].views.push_back(atBlock({480, 0, 1024, 480}, block)); // across/right of a tile edge
        groups[1].views.push_back(atBlock({512, 0, 992, 480}, block));  // anchor gap; not assumed empty
        groups[2].views.push_back(atBlock({-16, -16, 496, 496}, block));
    }
    return groups;
}

struct Sample { double us; Work w; std::size_t spans, ids; };
template<class T> void dist(const char* label, std::vector<T> values) {
    std::sort(values.begin(), values.end());
    auto q = [&](std::size_t percentile) { return values[(values.size() * percentile + 99) / 100 - 1]; };
    std::cout << ' ' << label << '=' << q(50) << '/' << q(95) << '/' << q(99) << '/' << values.back();
}
template<class F> void metric(const char* label, const std::vector<Sample>& samples, F f) {
    std::vector<decltype(f(samples.front()))> values;
    values.reserve(samples.size());
    for (const auto& s : samples) values.push_back(f(s));
    dist(label, std::move(values));
}
void printSamples(const Tree& tree, const Workload& g, const char* state, const std::vector<Sample>& v) {
    std::cout << "query tree=" << tree.name() << " workload=" << g.name << " cache=" << state
              << " n=" << v.size();
    metric("us", v, [](const Sample& s) { return s.us; });
    metric("nodes", v, [](const Sample& s) { return s.w.nodes; });
    metric("internal_tests", v, [](const Sample& s) { return s.w.internalTests; });
    metric("span_tests", v, [](const Sample& s) { return s.w.spanTests; });
    metric("page_refs", v, [](const Sample& s) { return s.w.refs; });
    metric("page_reads", v, [](const Sample& s) { return s.w.reads; });
    metric("read_bytes", v, [](const Sample& s) { return s.w.reads * pageBytes; });
    metric("span_hits", v, [](const Sample& s) { return s.spans; });
    metric("unique_id_hits", v, [](const Sample& s) { return s.ids; });
    const auto rereads = std::count_if(v.begin(), v.end(), [](const Sample& s) { return s.w.reads != 0; });
    std::cout << " queries_with_reads=" << rereads << '\n';
}

void benchmark(const std::array<Tree*, 2>& trees, Cache& cache, std::vector<Key>& frontier,
               const std::vector<Record>& records, const std::vector<Workload>& groups) {
    std::vector<Hit> actual, expected;
    std::size_t uniqueScratch = 0;
    for (const auto& g : groups) {
        std::array<std::array<std::vector<Sample>, 2>, 2> samples;
        std::vector<double> oracleUs;
        for (const auto& q : g.views) {
            // Collect the oracle timing without letting its first allocation
            // influence timed tree traversal; it is a separate in-memory control.
            const auto t = Clock::now();
            linear(records, q, expected);
            oracleUs.push_back(micros(t));
            actual.reserve(expected.size());
            const auto ids = uniqueIds(expected);
            uniqueScratch = std::max(uniqueScratch, bytes(ids));
            for (std::size_t order = 0; order < trees.size(); ++order) {
                const auto k = (order + oracleUs.size() - 1) % trees.size();
                auto& tree = *trees[k];
                cache.reset(tree.file);
                for (std::size_t state = 0; state < 2; ++state) {
                    const auto begin = Clock::now();
                    const auto w = tree.query(cache, q, frontier, actual);
                    const auto us = micros(begin);
                    check(actual, expected);
                    samples[k][state].push_back({us, w, expected.size(), ids.size()});
                }
            }
        }
        std::cout << "oracle phase=initial workload=" << g.name << " n=" << oracleUs.size()
                  << " records_tested_per_view=" << records.size();
        dist("us", std::move(oracleUs));
        std::cout << '\n';
        for (std::size_t k = 0; k < trees.size(); ++k) {
            printSamples(*trees[k], g, "app_cold", samples[k][0]);
            printSamples(*trees[k], g, "immediate_repeat", samples[k][1]);
        }
    }
    std::cout << "query_memory cache_B=" << cache.capacityBytes() << " cache_payload_B=" << Cache::payloadBytes
              << " frontier_capacity_B=" << bytes(frontier) << " span_output_capacity_B=" << bytes(actual)
              << " oracle_record_capacity_B=" << bytes(records) << " oracle_output_capacity_B=" << bytes(expected)
              << " id_dedup_scratch_peak_capacity_B=" << uniqueScratch
              << " (vector_capacity_ledger_not_process_peak; excludes stream/OS buffers and harness telemetry)\n";
}

void buildReport(const Tree& tree, const std::vector<Record>& records) {
    std::cout << "build tree=" << tree.name() << " us=" << tree.buildUs << " depth=" << tree.depth
              << " nodes=" << tree.logicalNodes << " file_pages=" << tree.file.pages
              << " file_bytes=" << tree.file.size() << " input_vector_capacity_B=" << bytes(records)
              << " peak_builder_vector_capacity_B=" << tree.buildVectorPeak
              << " serialization_page_scratch_B=" << pageBytes
              << " (capacity_ledger_excludes_stack_control_stream_OS)\n";
}

int run(const fs::path& input, const fs::path& out) {
    auto records = readInput(input);
    fs::create_directories(out);
    const auto packedPath = out / "m04_packed.pages";
    const auto dynamicPath = out / "m04_dynamic.pages";
    const auto hitsPath = out / "packed_hits.bin";
    const std::array paths{packedPath, dynamicPath, hitsPath};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!fs::exists(paths[i])) continue;
        if (fs::equivalent(input, paths[i])) throw std::runtime_error("output aliases input");
        for (std::size_t j = 0; j < i; ++j)
            if (fs::exists(paths[j]) && fs::equivalent(paths[i], paths[j]))
                throw std::runtime_error("output paths alias one another");
    }
    const auto runs = strokeRuns(records);
    std::cout << std::fixed << std::setprecision(3)
              << "scene spans=" << records.size() << " consecutive_stroke_id_runs=" << runs
              << " input_B=" << records.size() * sizeof(Record) << " page_B=" << pageBytes
              << " packed_fanout=" << fanout << " binary_nodes_per_page=" << nodesPerPage << '\n'
              << "method nearest_rank=p50/p95/p99/max; inclusive bounds; 256-page shared 64x4 LRU-by-set;"
              << " cold=application_cache_reset, NOT cold storage; repeat=cache_after_one_same_view;"
              << " page_refs include cache hits, page_reads and bytes are application file requests;"
              << " traversal+emit timed, sorting/oracle/cache_reset excluded from tree times;"
              << " query follows immediately after build/initial export; stream flush is not fsync\n";
    Tree packed(packedPath, true, records), dynamic(dynamicPath, false, records);
    buildReport(packed, records);
    buildReport(dynamic, records);
    const std::array trees{&packed, &dynamic};
    Cache cache;
    std::vector<Key> frontier;
    frontier.reserve(std::max(packed.frontierBound(), dynamic.frontierBound()));
    std::vector<Hit> expected, hits;
    linear(records, exportView, expected);
    cache.reset(packed.file);
    packed.query(cache, exportView, frontier, hits);
    check(hits, expected);
    {
        const auto ids = uniqueIds(hits);
        std::ofstream f(hitsPath, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot create packed_hits.bin");
        if (!ids.empty())
            f.write(reinterpret_cast<const char*>(ids.data()), static_cast<std::streamsize>(ids.size() * sizeof(ids[0])));
        f.close();
        if (!f) throw std::runtime_error("packed_hits.bin write failed");
        std::cout << "export file=packed_hits.bin viewport=112,112,368,368 spans=" << hits.size()
                  << " unique_ids=" << ids.size() << " order=numeric_int64_le\n";
    }
    // Prefer a span from the export viewport. For arbitrary finite input,
    // try other records if a 1024-unit positive X translation is not representable.
    Key target = none;
    Box old{}, moved{};
    auto choose = [&](Key k) {
        const auto b = records[k].box;
        const Box next{b.x0 + 1024, b.y0, b.x1 + 1024, b.y1};
        if (!valid(next) || next.x0 <= b.x0 || next.x1 <= b.x1) return false;
        target = k;
        old = b;
        moved = next;
        return true;
    };
    for (const auto& h : expected) if (choose(h.rank)) break;
    for (Key k = 0; target == none && k < records.size(); ++k) choose(k);
    if (target == none) throw std::runtime_error("no span can be translated by +1024 X in binary64");
    std::vector<Hit>().swap(expected);
    std::vector<Hit>().swap(hits);
    auto groups = workloads(records, runs);
    std::cout << "workloads complete_anchor_blocks_sampled=" << std::max(std::size_t{1}, runs / 256)
              << " sparse_n=128 boundary_n=48 full_block_n=16; positions repeat for small scenes\n";
    benchmark(trees, cache, frontier, records, groups);

    std::cout << "edit_target ordinal=" << target << " id=" << records[target].id
              << " translation_dx=1024 dy=0 old=" << old.x0 << ',' << old.y0 << ',' << old.x1 << ',' << old.y1
              << " new=" << moved.x0 << ',' << moved.y0 << ',' << moved.x1 << ',' << moved.y1 << '\n';
    for (auto* tree : trees) tree->edit(cache, target, old, moved);
    records[target].box = moved;
    const Box vacated{old.x0, old.y0, old.x0, old.y0};
    const Box acquired{moved.x1, moved.y1, moved.x1, moved.y1};
    if (!oracleIntersects(old, vacated) || oracleIntersects(moved, vacated) ||
        oracleIntersects(old, acquired) || !oracleIntersects(moved, acquired))
        throw std::runtime_error("edit endpoint probes do not distinguish old and new bounds");
    groups.push_back({"edit_endpoints", {vacated, acquired}});
    std::size_t postViews = 0;
    for (const auto& g : groups) {
        for (const auto& q : g.views) {
            linear(records, q, expected);
            for (auto* tree : trees) {
                cache.reset(tree->file);
                tree->query(cache, q, frontier, hits);
                check(hits, expected);
            }
            ++postViews;
        }
    }
    std::cout << "oracle_result=pass comparison=exact_span_ordinals_and_int64_ids"
              << " initial_views=" << postViews - 2 << " initial_cold_and_repeat_per_tree=yes"
              << " post_edit_views=" << postViews << " post_edit_cold_per_tree=yes"
              << " export_independently_checked=yes\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: m04_tree <validated-flat-spans.bin> <output-folder>\n";
        return 2;
    }
    try {
        return run(argv[1], argv[2]);
    } catch (const std::exception& e) {
        std::cerr << "m04_tree: " << e.what() << '\n';
        return 1;
    }
}
