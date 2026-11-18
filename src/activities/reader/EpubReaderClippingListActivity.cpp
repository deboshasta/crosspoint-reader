#include "EpubReaderClippingListActivity.h"

#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "ClippingStore.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "clippings/ClippingPreview.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

constexpr int ENTER_ACTIONS_MODE_MS = 700;
// Matches HEAP_HEADROOM in DictionaryDefinitionActivity.cpp, the other half
// of this flow.
constexpr size_t CLIP_HEAP_HEADROOM = 8 * 1024;

}  // namespace

EpubReaderClippingListActivity::EpubReaderClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("EpubReaderClippings", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

void EpubReaderClippingListActivity::onEnter() {
  UiListActivity::onEnter();
  initialListRender = true;
  rebuildRows();
}

int EpubReaderClippingListActivity::listCount() const { return static_cast<int>(CLIPPINGS.clippingCount()); }

const char* EpubReaderClippingListActivity::headerTitle() const { return tr(STR_CLIPPINGS); }

void EpubReaderClippingListActivity::rebuildRows() {
  windowStart = -1;
  windowCount = 0;
}

void EpubReaderClippingListActivity::refreshRowWindow(const int start) {
  const int total = listCount();
  int clamped = std::max(0, start);
  if (clamped > total - ROW_WINDOW) clamped = std::max(0, total - ROW_WINDOW);
  if (clamped == windowStart) return;

  windowCount = std::min(ROW_WINDOW, total - clamped);
  for (int slot = 0; slot < windowCount; ++slot) {
    const size_t clippingIndex = static_cast<size_t>(clamped + slot);
    const Clipping* clipping = CLIPPINGS.clippingAt(clippingIndex);
    previewLengths[slot] = 0;
    previews[slot][0] = '\0';
    if (!clipping || !CLIPPINGS.readClippingPreview(clippingIndex, previews[slot].data(), previews[slot].size(),
                                                    previewLengths[slot])) {
      LOG_ERR("CLIP", "Failed to read clipping %u", static_cast<unsigned>(clippingIndex));
    }

    fui::ListItem item;
    item.label = clipping && clipping->chapterTitle[0] != '\0' ? clipping->chapterTitle : tr(STR_CLIPPINGS);
    item.subtitle = previews[slot].data();
    item.icon = listIconFor(UIIcon::Bookmark, 32);
    item.actionValue = static_cast<int16_t>(clippingIndex);
    rowItems[slot] = item;
  }
  windowStart = clamped;
}

void EpubReaderClippingListActivity::showClippingActions() {
  if (confirmPopup.isActive() || nav.selected < 0 || nav.selected >= listCount()) return;
  const Clipping* clipping = CLIPPINGS.clippingAt(static_cast<size_t>(nav.selected));
  if (!clipping) return;
  static constexpr StrId ANCHORED_OPTIONS[] = {StrId::STR_OPEN, StrId::STR_GO_TO_PASSAGE, StrId::STR_DELETE};
  static constexpr StrId PLAIN_OPTIONS[] = {StrId::STR_OPEN, StrId::STR_DELETE};
  // Go to Passage needs the offsets the clipping was anchored at. Clippings
  // saved without them list Open and Delete only, which moves Delete up one.
  const bool anchored = clipping->startOffset != UINT32_MAX && clipping->endOffset != UINT32_MAX;
  confirmPopup.show(StrId::STR_CLIPPINGS, anchored ? ANCHORED_OPTIONS : PLAIN_OPTIONS, anchored ? 3 : 2, 0,
                    [this, anchored](const int index) {
                      const int deleteIndex = anchored ? 2 : 1;
                      if (index == 0) {
                        readSelected();
                      } else if (anchored && index == 1) {
                        goToSelectedPassage();
                      } else if (index == deleteIndex) {
                        showDeleteConfirmation();
                      }
                    });
  requestUpdate();
}

void EpubReaderClippingListActivity::goToSelectedPassage() {
  if (nav.selected < 0 || nav.selected >= listCount()) return;
  const Clipping* clipping = CLIPPINGS.clippingAt(static_cast<size_t>(nav.selected));
  if (!clipping) return;
  ProgressChangeResult result;
  result.spineIndex = clipping->spineIndex;
  result.page = clipping->startPage;
  result.hasVisibleTextOffset = true;
  result.visibleTextOffset = clipping->startOffset;
  setResult(std::move(result));
  finish();
}

bool EpubReaderClippingListActivity::readClippingInto(const int index, std::string& title, std::string& text) {
  if (index < 0 || index >= listCount()) return false;
  const Clipping* clipping = CLIPPINGS.clippingAt(static_cast<size_t>(index));
  if (!clipping) return false;
  // readClippingText resizes `text` to the stored length, and the build has
  // exceptions disabled, so a failed allocation aborts rather than returning.
  // Sibling browsing reads here while the open clipping is still resident, so
  // check the heap first the way the rest of this flow does.
  const size_t need = static_cast<size_t>(clipping->textLength) + 1;
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.largestBlockBytes < need || heap.freeBytes < need + CLIP_HEAP_HEADROOM) {
    LOG_ERR("CLIP", "Low heap for clipping text (%u bytes)", static_cast<unsigned>(need));
    return false;
  }
  if (!CLIPPINGS.readClippingText(static_cast<size_t>(index), text)) {
    LOG_ERR("CLIP", "Failed to read clipping text %d", index);
    return false;
  }
  title = clipping->chapterTitle[0] != '\0' ? clipping->chapterTitle : tr(STR_CLIPPINGS);
  return true;
}

void EpubReaderClippingListActivity::readSelected() {
  std::string title;
  std::string text;
  if (!readClippingInto(nav.selected, title, text)) return;
  auto detail =
      makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(title), std::move(text));
  if (!detail) {
    LOG_ERR("CLIP", "Failed to allocate clipping detail activity");
    return;
  }
  // Swiping in the detail view walks this list. Capturing `this` is safe
  // because the parent stays on the activity stack under the child, and the
  // selection follows along so Back returns to the clipping last read.
  detail->enableBrowse(
      [this](const int delta, std::string& nextTitle, std::string& nextText) {
        // Read only. The viewer commits the cursor once the sibling is laid
        // out; moving it here would strand the list on a row that is not on
        // screen if that fails.
        return readClippingInto(nav.selected + delta, nextTitle, nextText);
      },
      [this](const int delta) {
        // requestSelection is the input-task API: it stores the selection at
        // once and leaves the render-owned viewport pull to the next build.
        nav.requestSelection(nav.selected + delta);
      });
  startActivityForResult(std::move(detail), [this](const ActivityResult&) { requestUpdate(); });
}

void EpubReaderClippingListActivity::activateIndex(const int index) {
  if (confirmPopup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  // A tap reads the clipping straight away; the action list is on long press.
  readSelected();
}

void EpubReaderClippingListActivity::onRowLongPress(const int index) {
  if (confirmPopup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  showClippingActions();
}

void EpubReaderClippingListActivity::showDeleteConfirmation() {
  if (listCount() == 0 || confirmPopup.isActive()) return;
  confirmingDelete = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup.show(tr(STR_DELETE_CLIPPING_CONFIRM), options, 2, 0, [this](const int index) {
    confirmingDelete = false;
    if (index == 1) deleteSelected();
    requestUpdate();
  });
  requestUpdate();
}

void EpubReaderClippingListActivity::deleteSelected() {
  if (nav.selected < 0 || nav.selected >= listCount()) return;
  if (!CLIPPINGS.removeClippingAt(static_cast<size_t>(nav.selected))) {
    LOG_ERR("CLIP", "Failed to delete clipping %d", static_cast<int>(nav.selected));
    return;
  }
  rebuildRows();
  if (listCount() == 0) {
    finish();
    return;
  }
  if (nav.selected >= listCount()) nav.selected = listCount() - 1;
  nav.follow(listCount());
}

bool EpubReaderClippingListActivity::handleCustomInput() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete) {
    confirmingDelete = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool EpubReaderClippingListActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, ENTER_ACTIONS_MODE_MS)) {
    showClippingActions();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    readSelected();
    return true;
  }
  return false;
}

void EpubReaderClippingListActivity::drawFooter() {
  // The help text reads "Hold Open for Actions", so the confirm key has to be
  // labelled Open on this screen. LibraryListActivity pairs the same hint with
  // the same label for the same reason; the base footer says Select.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EpubReaderClippingListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (listCount() == 0) {
    screen.centeredText(tr(STR_NO_CLIPPINGS), screen.theme().bodyText);
    return;
  }

  if (!mappedInput.hasTouch()) {
    const int helpHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpHeight},
                     tr(STR_HOLD_OPEN_FOR_ACTIONS));
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  refreshRowWindow(nav.top);
  props.items = rowItems.data();
  props.itemsWindowFirst = static_cast<uint16_t>(windowStart);
  props.itemsWindowCount = static_cast<uint16_t>(windowCount);
  screen.list(props);
}

void EpubReaderClippingListActivity::render(RenderLock&& lock) {
  if (confirmPopup.processRender(renderer, mappedInput)) return;
  if (initialListRender && listCount() > 0) {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  }
  initialListRender = false;
  UiListActivity::render(std::move(lock));
}
