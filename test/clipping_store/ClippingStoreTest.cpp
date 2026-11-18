#include <ClippingStore.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

namespace {
using Result = ClippingStore::AddResult;
Result addAt(ClippingStore& store, uint32_t startOffset, uint32_t endOffset, const std::string& text = "one",
             const char* title = "Chapter", uint32_t replaceStartOffset = UINT32_MAX,
             uint32_t replaceEndOffset = UINT32_MAX) {
  return store.addClipping(0, 0, 0, 1, 0, 0, 1, title, 0, text, 1, startOffset, endOffset, replaceStartOffset,
                           replaceEndOffset);
}
// Every call gets its own non-overlapping range. Two clippings over identical
// offsets are the same passage, which addClipping now consolidates into one --
// correct, but not what the tests below are about, so they must not collide.
unsigned addSeq = 0;
Result add(ClippingStore& store, const std::string& text = "one", const char* title = "Chapter") {
  const uint32_t start = addSeq++ * 4;
  return addAt(store, start, start + 3, text, title);
}
std::string storePath() {
  for (const auto& [path, node] : fake::files)
    if (path.ends_with(".bin")) return path;
  return "";
}
}  // namespace

TEST(ClippingStore, FailedLoadCannotPublishOrOverwriteValidPrefix) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  ASSERT_EQ(add(store, "two"), Result::Added);
  store.unload();
  auto& bytes = fake::files.at(storePath())->bytes;
  bytes.pop_back();
  const auto damaged = bytes;
  EXPECT_FALSE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(store.clippingCount(), 0u);
  EXPECT_EQ(add(store), Result::SaveFailed);
  EXPECT_FALSE(store.saveToFile());
  EXPECT_FALSE(store.prepareSync());
  store.unload();
  EXPECT_EQ(bytes, damaged);
  ASSERT_TRUE(store.loadForBook("/other.epub", "Other", "", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  EXPECT_FALSE(store.loadForBook("/bad.txt", "", "", "unsupported"));
  EXPECT_EQ(store.clippingCount(), 0u);
  EXPECT_EQ(add(store), Result::SaveFailed);
}

TEST(ClippingStore, AllocationFailureKeepsOldIndexAndUnloadReleasesIt) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  for (int i = 0; i < 16; ++i) ASSERT_EQ(add(store), Result::Added);
  fake::failAlloc = 0;
  EXPECT_EQ(add(store), Result::SaveFailed);
  EXPECT_EQ(store.clippingCount(), 16u);
  std::string text;
  EXPECT_TRUE(store.readClippingText(3, text));
  EXPECT_EQ(text, "one");
  store.unload();
  EXPECT_EQ(store.clippingAt(0), nullptr);
  EXPECT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(store.clippingCount(), 16u);
}

TEST(ClippingStore, CountLimitAndHeaderLimitsRoundTrip) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  for (unsigned i = 0; i < CLIPPING_MAX_PER_BOOK - 1; ++i) ASSERT_EQ(add(store), Result::Added);
  store.unload();
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(add(store), Result::Added);
  EXPECT_EQ(add(store), Result::LimitReached);
  store.unload();
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(store.clippingCount(), CLIPPING_MAX_PER_BOOK);
  const auto original = fake::files.at(storePath())->bytes;
  store.unload();
  ASSERT_TRUE(store.loadForBook("/book.epub", std::string(4097, 'x'), "Author", "epub"));
  EXPECT_FALSE(store.removeClippingAt(0));
  store.unload();
  EXPECT_EQ(fake::files.at(storePath())->bytes, original);
}

TEST(ClippingStore, GrowthKeepsRecordsInPlaceAndDeletionRollsBackAcrossBlocks) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  const Clipping* first = nullptr;
  for (unsigned i = 0; i < 33; ++i) {
    Clipping clipping;
    snprintf(clipping.id, sizeof(clipping.id), "clip-%u", i);
    const std::string text = std::to_string(i);
    clipping.textLength = text.size();
    ASSERT_TRUE(store.applyRemote(clipping, text, false));
    if (i == 0) first = store.clippingAt(0);
    EXPECT_EQ(store.clippingAt(0), first);
  }
  const auto path = storePath();
  const auto original = fake::files.at(path)->bytes;
  // The journal succeeds; the store rewrite fails after records have shifted.
  fake::failWrite = 1;
  EXPECT_FALSE(store.removeClippingAt(15));
  ASSERT_EQ(store.clippingCount(), 33u);
  EXPECT_EQ(fake::files.at(path)->bytes, original);
  for (unsigned i = 0; i < 33; ++i) {
    std::string text;
    ASSERT_TRUE(store.readClippingText(i, text));
    EXPECT_EQ(text, std::to_string(i));
  }
  const Clipping removed = *store.clippingAt(15);
  fake::failWrite = 0;
  EXPECT_FALSE(store.applyRemote(removed, "", true));
  EXPECT_EQ(store.clippingCount(), 33u);
  ASSERT_TRUE(store.applyRemote(removed, "", true));
  ASSERT_TRUE(store.removeClippingAt(15));
  store.unload();
  // Fail partway through allocating blocks while loading, then retry.
  fake::failAlloc = 1;
  EXPECT_FALSE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(store.clippingCount(), 0u);
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(store.clippingCount(), 31u);
  for (unsigned i = 0; i < 31; ++i) {
    std::string text;
    ASSERT_TRUE(store.readClippingText(i, text));
    EXPECT_EQ(text, std::to_string(i < 15 ? i : i + 2));
  }
}

TEST(ClippingStore, OffsetAllocationFailureDoesNotCreateTempFile) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  const auto path = storePath();
  const auto original = fake::files.at(path)->bytes;
  fake::failAlloc = 0;
  EXPECT_EQ(add(store, "two"), Result::SaveFailed);
  EXPECT_EQ(store.clippingCount(), 1u);
  EXPECT_EQ(fake::files.at(path)->bytes, original);
  EXPECT_FALSE(Storage.exists((path + ".tmp").c_str()));
  EXPECT_EQ(add(store, "two"), Result::Added);
}

TEST(ClippingStore, ShortDeletionAppendRollsBackAndRetryCanSync) {
  for (const bool failedRollback : {false, true}) {
    fake::reset();
    ClippingStore store;
    ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
    ASSERT_EQ(add(store), Result::Added);
    ASSERT_EQ(add(store, "two"), Result::Added);
    ASSERT_TRUE(store.prepareSync());
    const std::string firstId = store.clippingAt(0)->id;
    const std::string secondId = store.clippingAt(1)->id;
    ASSERT_TRUE(store.removeClippingAt(0));
    const auto journalPath = storePath() + ".deleted";
    const auto original = fake::files.at(journalPath)->bytes;
    fake::shortWrite = 1;
    if (failedRollback) fake::failTruncate = 0;
    EXPECT_FALSE(store.removeClippingAt(0));
    ASSERT_EQ(store.clippingCount(), 1u);
    if (failedRollback) {
      EXPECT_EQ(fake::files.at(journalPath)->bytes.size(), original.size() + 1);
    } else {
      EXPECT_EQ(fake::files.at(journalPath)->bytes, original);
    }
    ASSERT_TRUE(store.removeClippingAt(0));
    store.unload();
    ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
    uint32_t offset = 0;
    char id[65];
    ASSERT_TRUE(store.nextDeletion(offset, id));
    EXPECT_EQ(id, firstId);
    ASSERT_TRUE(store.nextDeletion(offset, id));
    EXPECT_EQ(id, secondId);
    ASSERT_TRUE(store.nextDeletion(offset, id));
    EXPECT_EQ(id[0], '\0');
    EXPECT_EQ(fake::files.at(journalPath)->bytes.size(), 130u);
    EXPECT_TRUE(store.finishDeletions());
  }
}

TEST(ClippingStore, InterruptedDeletionTailRecoversBeforeReading) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  ASSERT_TRUE(store.prepareSync());
  const std::string expectedId = store.clippingAt(0)->id;
  ASSERT_TRUE(store.removeClippingAt(0));
  const auto journalPath = storePath() + ".deleted";
  const auto original = fake::files.at(journalPath)->bytes;
  fake::files.at(journalPath)->bytes.push_back('x');
  store.unload();
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  uint32_t offset = 0;
  char id[65];
  fake::failTruncate = 0;
  EXPECT_FALSE(store.nextDeletion(offset, id));
  EXPECT_EQ(offset, 0u);
  ASSERT_TRUE(store.nextDeletion(offset, id));
  EXPECT_EQ(id, expectedId);
  EXPECT_EQ(fake::files.at(journalPath)->bytes, original);
  ASSERT_TRUE(store.nextDeletion(offset, id));
  EXPECT_EQ(id[0], '\0');
  ASSERT_TRUE(store.finishDeletions());
  fake::add(journalPath, "x");
  offset = 0;
  ASSERT_TRUE(store.nextDeletion(offset, id));
  EXPECT_EQ(id[0], '\0');
  EXPECT_TRUE(fake::files.at(journalPath)->bytes.empty());
}

TEST(ClippingStore, Utf8TitleAndFailedDeletionPreserveRecord) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(add(store, "one", (std::string(46, 'x') + "가").c_str()), Result::Added);
  EXPECT_EQ(std::string(store.clippingAt(0)->chapterTitle), std::string(46, 'x'));
  fake::failWrite = 0;
  EXPECT_FALSE(store.removeClippingAt(0));
  ASSERT_EQ(store.clippingCount(), 1u);
  std::string text;
  EXPECT_TRUE(store.readClippingText(0, text));
  EXPECT_EQ(text, "one");
}

TEST(ClippingStore, MovesFolderWithClipsAndDeletionJournalAndRollsBackFailure) {
  for (const int failure : {-1, 0, 1, 2, 3}) {
    fake::reset();
    fake::add("/books/sub/book.epub");
    ClippingStore store;
    ASSERT_TRUE(store.loadForBook("/books/sub/book.epub", "Book", "Author", "epub"));
    ASSERT_EQ(add(store), Result::Added);
    ASSERT_EQ(add(store, "two"), Result::Added);
    ASSERT_TRUE(store.prepareSync());
    ASSERT_TRUE(store.removeClippingAt(0));
    store.unload();
    const auto oldStore = storePath();
    const auto original = fake::files.at(oldStore)->bytes;
    const auto deleted = fake::files.at(oldStore + ".deleted")->bytes;
    fake::failRename = failure;
    const bool moved = ClippingStore::moveBook("/books", "/renamed");
    if (moved) {
      EXPECT_FALSE(Storage.exists("/books/sub/book.epub"));
      EXPECT_TRUE(Storage.exists("/renamed/sub/book.epub"));
      EXPECT_FALSE(Storage.exists(oldStore.c_str()));
      ASSERT_TRUE(store.loadForBook("/renamed/sub/book.epub", "", "", "epub"));
      EXPECT_EQ(store.clippingCount(), 1u);
      std::string text;
      EXPECT_TRUE(store.readClippingText(0, text));
      EXPECT_EQ(text, "two");
      uint32_t offset = 0;
      char id[65];
      EXPECT_TRUE(store.nextDeletion(offset, id));
      EXPECT_NE(id[0], 0);
    } else {
      EXPECT_TRUE(Storage.exists("/books/sub/book.epub"));
      EXPECT_FALSE(Storage.exists("/renamed/sub/book.epub"));
      EXPECT_EQ(fake::files.at(oldStore)->bytes, original);
      EXPECT_EQ(fake::files.at(oldStore + ".deleted")->bytes, deleted);
    }
  }
}

TEST(ClippingStore, MoveRefusesExistingDestinationHistoryAndDeleteCleansSidecars) {
  fake::reset();
  fake::add("/one.epub");
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/one.epub", "One", "", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  store.unload();
  ASSERT_TRUE(store.loadForBook("/two.epub", "Two", "", "epub"));
  ASSERT_EQ(add(store, "two"), Result::Added);
  store.unload();
  EXPECT_FALSE(ClippingStore::moveBook("/one.epub", "/two.epub"));
  EXPECT_TRUE(Storage.exists("/one.epub"));
  EXPECT_TRUE(ClippingStore::deleteForFilePath("/two.epub", "epub"));
  EXPECT_TRUE(ClippingStore::moveBook("/one.epub", "/two.epub"));
  const auto path = storePath();
  fake::add(path + ".deleted", std::string(65, 'x'));
  fake::add(path + ".bak");
  fake::add(path + ".tmp");
  EXPECT_TRUE(ClippingStore::deleteForFilePath("/two.epub", "epub"));
  EXPECT_FALSE(Storage.exists(path.c_str()));
  EXPECT_FALSE(Storage.exists((path + ".deleted").c_str()));
  EXPECT_FALSE(Storage.exists((path + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((path + ".tmp").c_str()));
}

TEST(ClippingStore, OverlappingClippingsConsolidateIntoOne) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  // "2" and "4" clipped separately, then "1 2 3 4" over both: one clipping.
  ASSERT_EQ(addAt(store, 2, 3, "2"), Result::Added);
  ASSERT_EQ(addAt(store, 6, 7, "4"), Result::Added);
  ASSERT_EQ(store.clippingCount(), 2u);
  ASSERT_EQ(addAt(store, 0, 8, "1 2 3 4"), Result::Added);
  EXPECT_EQ(store.clippingCount(), 1u);
  std::string text;
  ASSERT_TRUE(store.readClippingText(0, text));
  EXPECT_EQ(text, "1 2 3 4");
}

TEST(ClippingStore, ClippingInsideAnExistingOneIsNotStoredAgain) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(addAt(store, 0, 8, "1 2 3 4"), Result::Added);
  // The passage is already clipped, so nothing is stored and the caller must
  // not export it again.
  EXPECT_EQ(addAt(store, 2, 3, "2"), Result::AlreadyCovered);
  EXPECT_EQ(store.clippingCount(), 1u);
  std::string text;
  ASSERT_TRUE(store.readClippingText(0, text));
  EXPECT_EQ(text, "1 2 3 4");
}

TEST(ClippingStore, EditingAClippingReplacesItEvenWhenShrunk) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(addAt(store, 0, 8, "1 2 3 4"), Result::Added);
  // A shrunk edit sits inside the clipping it came from, so without naming the
  // range it replaces it would be rejected as AlreadyCovered and lost.
  ASSERT_EQ(addAt(store, 0, 4, "1 2", "Chapter", 0, 8), Result::Added);
  EXPECT_EQ(store.clippingCount(), 1u);
  std::string text;
  ASSERT_TRUE(store.readClippingText(0, text));
  EXPECT_EQ(text, "1 2");
}

TEST(ClippingStore, AdjacentClippingsStaySeparate) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  // Touching at a boundary is adjacency, not overlap.
  ASSERT_EQ(addAt(store, 0, 4, "1 2"), Result::Added);
  ASSERT_EQ(addAt(store, 4, 8, "3 4"), Result::Added);
  EXPECT_EQ(store.clippingCount(), 2u);
}

TEST(ClippingStore, PartialOverlapIsLeftForTheSelectionToWiden) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  // The store absorbs containment only. ClipSelectionActivity widens a partial
  // overlap into a containment before it reaches here, because the union's text
  // exists in neither stored clipping. Two clippings is the honest outcome at
  // this level; the merge happens a layer up.
  ASSERT_EQ(addAt(store, 0, 5, "1 2"), Result::Added);
  ASSERT_EQ(addAt(store, 3, 8, "2 3"), Result::Added);
  EXPECT_EQ(store.clippingCount(), 2u);
}

TEST(ClippingStore, UnanchoredClippingsNeverConsolidate) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  // UINT32_MAX means "source position unknown", not a huge offset, so these
  // cannot be compared and must both survive.
  ASSERT_EQ(addAt(store, UINT32_MAX, UINT32_MAX, "one"), Result::Added);
  ASSERT_EQ(addAt(store, UINT32_MAX, UINT32_MAX, "two"), Result::Added);
  EXPECT_EQ(store.clippingCount(), 2u);
}

TEST(ClippingStore, FailedAbsorbingAddKeepsTheClippingsItWouldHaveSwallowed) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(addAt(store, 2, 3, "2"), Result::Added);
  ASSERT_EQ(addAt(store, 6, 7, "4"), Result::Added);
  ASSERT_EQ(store.clippingCount(), 2u);
  // Absorbing is irreversible: it flushes each id to the sync deletion journal
  // and rewrites the store. So the replacement must be durable first, and a
  // write failure must leave both originals exactly where they were.
  // NOTE: this covers the write-failure rollback, NOT the ordering. fake::fail
  // is a one-shot countdown, so failWrite=1 aborts inside the first removal's
  // own rewrite, which rolled itself back even before absorbing was moved after
  // the write. OversizeTextIsRefusedBeforeAnythingIsAbsorbed is the test that
  // actually discriminates.
  fake::failWrite = 1;
  EXPECT_EQ(addAt(store, 0, 8, "1 2 3 4"), Result::SaveFailed);
  fake::failWrite = 0;
  ASSERT_EQ(store.clippingCount(), 2u);
  std::string text;
  ASSERT_TRUE(store.readClippingText(0, text));
  EXPECT_EQ(text, "2");
  ASSERT_TRUE(store.readClippingText(1, text));
  EXPECT_EQ(text, "4");
  // Reloading proves the file on disk still holds both, not just memory.
  store.unload();
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(store.clippingCount(), 2u);
}

TEST(ClippingStore, OversizeTextIsRefusedBeforeAnythingIsAbsorbed) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(addAt(store, 2, 3, "2"), Result::Added);
  ASSERT_EQ(addAt(store, 6, 7, "4"), Result::Added);
  // The ordering test. Text length is checked BEFORE the absorb loop, so this
  // refusal costs nothing. When it was checked after, both clippings were
  // already deleted and journalled for deletion on every synced device, and
  // this returned SaveFailed with a count of 0.
  EXPECT_EQ(addAt(store, 0, 8, std::string(CLIPPING_TEXT_MAX + 1, 'x')), Result::SaveFailed);
  EXPECT_EQ(store.clippingCount(), 2u);
  std::string text;
  ASSERT_TRUE(store.readClippingText(0, text));
  EXPECT_EQ(text, "2");
  ASSERT_TRUE(store.readClippingText(1, text));
  EXPECT_EQ(text, "4");
}

TEST(ClippingStore, AbsorbingIsRefusedAtTheLimitRatherThanMakingRoom) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  // Taken BEFORE the loop so it is the first range the loop stores. addSeq is
  // cumulative and fake::reset() does not clear it, so a hardcoded range would
  // cover nothing when the suite runs in one process -- the cap would fire for
  // the trivial reason and this would pass while asserting nothing.
  const uint32_t base = addSeq * 4;
  for (unsigned i = 0; i < CLIPPING_MAX_PER_BOOK; ++i) ASSERT_EQ(add(store), Result::Added);
  ASSERT_EQ(store.clippingCount(), CLIPPING_MAX_PER_BOOK);
  // A clipping covering stored ones could free slots by absorbing them, but
  // only by destroying them before the replacement is known to be storable.
  // Refusing is the honest answer; the reader reports the limit and the user
  // deletes clippings from View Clippings.
  EXPECT_EQ(addAt(store, base, base + 8, "1 2 3 4"), Result::LimitReached);
  EXPECT_EQ(store.clippingCount(), CLIPPING_MAX_PER_BOOK);
}
