#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ClipSelectionActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        content{.headword = std::move(headword), .text = std::move(definition)},
        htmlDefinition(htmlDefinition) {}

  bool enableLookup(Dictionary& dictionary, std::string&& query);

  // Opt-in sibling browsing for list-backed viewers (the clippings list).
  // delta is +1 next / -1 previous. Left unset by dictionary lookups, which
  // keeps horizontal swipes unused there as before.
  //
  // read writes the sibling's title and text and returns false at either end.
  // It must not move the owner's cursor: the layout can still fail after it
  // returns, and commit runs only once the sibling is actually on screen.
  // Splitting the two is what keeps the list and the viewer from disagreeing
  // about which item is open.
  //
  // Only sized for plain-text owners. The memory argument in browseSibling
  // assumes one 8-byte span per line; an owner built with htmlDefinition would
  // lay out styled pages at peak coexistence and needs that budget redone.
  using BrowseHandler = std::function<bool(int delta, std::string& title, std::string& text)>;
  using BrowseCommit = std::function<void(int delta)>;
  void enableBrowse(BrowseHandler read, BrowseCommit commit) {
    browse = std::move(read);
    browseCommit = std::move(commit);
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct PlainSelectionLine;

  struct Line {
    uint32_t start;
    uint16_t len;
    bool bold;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };

  BodyArea bodyArea() const;

  struct Definition {
    std::string headword;
    std::string text;
    std::vector<std::unique_ptr<Page>> pages;
    std::vector<Line> lines;
    int currentPage = 0;
    int totalPages = 1;
    int linesPerPage = 1;
    int selectionPage = 0;
    ClipSelectionActivity::Selection selection;

    void releasePayload() {
      std::string{}.swap(text);
      std::vector<std::unique_ptr<Page>>{}.swap(pages);
      std::vector<Line>{}.swap(lines);
    }
  };

  // Replay the original query: the displayed headword may be a stem or synonym.
  struct HistoryEntry {
    std::string query;
    int page = 0;
    int selectionPage = 0;
    ClipSelectionActivity::Selection selection;
    bool styled = false;
  };

  struct LookupState {
    explicit LookupState(Dictionary& dictionary) : dictionary(dictionary) {}
    Dictionary& dictionary;  // Owned by the retained DictionaryWordSelectActivity parent.
    std::array<HistoryEntry, 16> history;
    size_t depth = 0;
    std::string query;
    std::string pendingWord;
    std::vector<std::unique_ptr<Page>> selectionPages;
  };

  bool prepareDefinition(Definition& next, bool forcePlain = false);
  bool layoutHtmlPages(Definition& next);
  bool wrapText(Definition& next);
  bool reserveLines(Definition& next, size_t count);
  int measureSpan(int fontId, const char* text, size_t len, EpdFontFamily::Style family = EpdFontFamily::REGULAR) const;
  void drawBody(int fontId, int x, int startY, int currentPage) const;
  void startSelection(int x = -1, int y = -1);
  bool buildPlainSelectionPages();
  void navigateDefinition(bool back);
  bool browseSibling(int delta);
  bool restoreCurrentDefinition(bool styled);
  void showMessage(StrId message);

  Definition content;
  const bool htmlDefinition;
  std::unique_ptr<LookupState> lookup;
  BrowseHandler browse;
  BrowseCommit browseCommit;
  StrId message = StrId::STR_DICT_NOT_FOUND;
  unsigned long messageTime = 0;
  bool messageVisible = false;
  bool initialLayoutFailed = false;
  ButtonNavigator buttonNavigator;
};
