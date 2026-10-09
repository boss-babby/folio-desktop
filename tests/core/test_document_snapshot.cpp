// Work package B1 acceptance test.
//
// "Append 3 deltas; read() at any offset matches a flat concatenation;
//  fuzz 10k random reads."
//
// Standalone (no GoogleTest dependency yet) so it runs in any toolchain.

#include "folio/core/engine/DocumentSnapshot.h"
#include "folio/core/render/TileKey.h"

#include <cassert>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace folio::core;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::vector<std::byte> bytesOf(const std::string& s) {
    std::vector<std::byte> v(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
        v[i] = static_cast<std::byte>(s[i]);
    return v;
}

std::string readAll(const DocumentSnapshot& s) {
    std::vector<std::byte> buf(s.size());
    const std::size_t n = s.read(0, buf);
    std::string out(n, '\0');
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<char>(buf[i]);
    return out;
}

// --- B1: rope correctness -------------------------------------------------

void test_rope_matches_flat_concatenation() {
    std::printf("B1  rope == flat concatenation\n");

    const std::string base  = "%PDF-1.7\n<base document bytes>\n";
    const std::string d1    = "<<incremental update one>>\n";
    const std::string d2    = "<<incremental update two>>\n";
    const std::string d3    = "<<incremental update three>>\n";

    auto s0 = DocumentSnapshot::Builder::fromBase(bytesOf(base));
    auto s1 = DocumentSnapshot::Builder::append(s0, bytesOf(d1), {3});
    auto s2 = DocumentSnapshot::Builder::append(s1, bytesOf(d2), {7});
    auto s3 = DocumentSnapshot::Builder::append(s2, bytesOf(d3), {3, 11});

    check(s0->generation() == 0, "gen 0");
    check(s3->generation() == 3, "gen 3 after three appends");

    const std::string expected = base + d1 + d2 + d3;
    check(s3->size() == expected.size(), "size == sum of parts");
    check(readAll(*s3) == expected, "full read matches concatenation");

    // Earlier generations stay valid and unchanged - PDFium may still be
    // serving tiles from gen N while gen N+1 warms up.
    check(readAll(*s0) == base, "gen 0 still intact after appends");
    check(readAll(*s1) == base + d1, "gen 1 still intact");
}

void test_reads_at_every_offset() {
    std::printf("B1  read() at every offset and length\n");

    const std::string base = "ABCDEFGHIJ";
    const std::string d1   = "klmnop";
    const std::string d2   = "0123";

    auto s = DocumentSnapshot::Builder::append(
        DocumentSnapshot::Builder::append(
            DocumentSnapshot::Builder::fromBase(bytesOf(base)), bytesOf(d1), {1}),
        bytesOf(d2), {2});

    const std::string flat = base + d1 + d2;
    bool allOk = true;

    for (std::size_t pos = 0; pos <= flat.size() + 2; ++pos) {
        for (std::size_t len = 0; len <= flat.size() + 2; ++len) {
            std::vector<std::byte> buf(len);
            const std::size_t got = s->read(pos, buf);

            const std::size_t want =
                (pos >= flat.size()) ? 0 : std::min(len, flat.size() - pos);
            if (got != want) { allOk = false; continue; }

            for (std::size_t i = 0; i < got; ++i) {
                if (static_cast<char>(buf[i]) != flat[pos + i]) {
                    allOk = false;
                    break;
                }
            }
        }
    }
    check(allOk, "every (offset, length) pair matches the flat buffer");
}

void test_fuzz_10k_random_reads() {
    std::printf("B1  fuzz 10k random reads across 12 segments\n");

    std::mt19937 rng(0xF0110);
    std::string flat;
    SnapshotPtr snap;

    for (int seg = 0; seg < 12; ++seg) {
        const std::size_t len = 1 + (rng() % 500);
        std::string chunk(len, '\0');
        for (auto& c : chunk) c = static_cast<char>('a' + (rng() % 26));
        flat += chunk;
        snap = seg == 0
                   ? DocumentSnapshot::Builder::fromBase(bytesOf(chunk))
                   : DocumentSnapshot::Builder::append(
                         snap, bytesOf(chunk),
                         {static_cast<PageId>(rng() % 24)});
    }

    check(snap->size() == flat.size(), "fuzz: total size");

    bool allOk = true;
    for (int i = 0; i < 10000; ++i) {
        const std::size_t pos = rng() % (flat.size() + 32);
        const std::size_t len = rng() % 2048;
        std::vector<std::byte> buf(len);
        const std::size_t got = snap->read(pos, buf);

        const std::size_t want =
            (pos >= flat.size()) ? 0 : std::min(len, flat.size() - pos);
        if (got != want) { allOk = false; break; }
        for (std::size_t k = 0; k < got; ++k) {
            if (static_cast<char>(buf[k]) != flat[pos + k]) {
                allOk = false;
                break;
            }
        }
        if (!allOk) break;
    }
    check(allOk, "10k random reads all correct");
}

void test_dirty_page_accumulation() {
    std::printf("B1  dirty-page accumulation across generations\n");

    auto s0 = DocumentSnapshot::Builder::fromBase(bytesOf("base"));
    auto s1 = DocumentSnapshot::Builder::append(s0, bytesOf("d1"), {3});
    auto s2 = DocumentSnapshot::Builder::append(s1, bytesOf("d2"), {7});
    auto s3 = DocumentSnapshot::Builder::append(s2, bytesOf("d3"), {3, 11});

    // From gen 2 only the last step matters.
    const auto& from2 = s3->dirtyPagesSince(2);
    check(from2.size() == 2 && from2.count(3) && from2.count(11),
          "dirtySince(2) == {3, 11}");

    // From gen 0 everything touched since must be invalidated - this is the
    // bug that would silently serve stale tiles if accumulation were missing.
    const auto& from0 = s3->dirtyPagesSince(0);
    check(from0.size() == 3 && from0.count(3) && from0.count(7) &&
              from0.count(11),
          "dirtySince(0) == {3, 7, 11} (accumulated)");

    check(s3->dirtyPagesSince(3).empty(), "dirtySince(current) is empty");
    check(!s3->isCleanSince(0), "not clean since gen 0");
    check(s3->isCleanSince(3), "clean since own generation");
}

// --- TileKey --------------------------------------------------------------

void test_tilekey_layout_and_quantisation() {
    std::printf("C1  TileKey layout and zoom quantisation\n");

    check(sizeof(TileKey) == 16, "TileKey is 16 bytes");

    TileKey a{}; a.pageId = 5; a.tileX = 2; a.tileY = 3;
    a.scaleQ = quantiseScale(1.25f);
    TileKey b = a;
    check(a == b, "equality");
    check(std::hash<TileKey>{}(a) == std::hash<TileKey>{}(b), "hash stable");

    b.tileY = 4;
    check(!(a == b), "inequality on tileY");

    // The point of quantisation: nearby zooms collapse to one bucket, so a
    // pinch gesture does not generate hundreds of cache generations.
    check(quantiseScale(1.25f) == quantiseScale(1.27f),
          "1.25 and 1.27 share a bucket");
    check(quantiseScale(1.25f) != quantiseScale(1.40f),
          "1.25 and 1.40 differ");
    check(dequantiseScale(quantiseScale(1.25f)) == 1.25f,
          "exact multiples round-trip");

    int buckets = 0, last = -1;
    for (float z = 1.0f; z <= 2.0f; z += 0.01f) {
        const int q = quantiseScale(z);
        if (q != last) { ++buckets; last = q; }
    }
    check(buckets <= 10, "100 zoom samples over 1.0-2.0 -> <=10 buckets");
    std::printf("      (%d buckets for 100 distinct zoom levels)\n", buckets);
}

}  // namespace

int main() {
    std::printf("\nFolio core tests\n================\n\n");

    test_rope_matches_flat_concatenation();
    test_reads_at_every_offset();
    test_fuzz_10k_random_reads();
    test_dirty_page_accumulation();
    test_tilekey_layout_and_quantisation();

    std::printf("\n%s  (%d failure%s)\n\n",
                g_failures == 0 ? "ALL PASS" : "FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
