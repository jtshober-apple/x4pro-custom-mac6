#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/ContextMenuActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;

// Derive a display title from a bare filename (no path, no extension).
// Used to seed the RecentBooks title after a user rename so it shows on
// the home screen immediately — before any network metadata fetch.
std::string titleFromFilename(const std::string& filename) {
  // Strip trailing slash for directories
  std::string name = filename;
  if (!name.empty() && name.back() == '/') name.pop_back();
  // Strip extension
  const auto dot = name.rfind('.');
  if (dot != std::string::npos) name = name.substr(0, dot);
  // Replace underscores/hyphens with spaces for readability
  for (char& c : name) {
    if (c == '_' || c == '-') c = ' ';
  }
  return name;
}
}  // namespace

std::string getFileName(std::string filename);
std::string getFileExtension(const std::string& filename);

FileBrowserActivity::FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string initialPath, const Mode mode)
    : UiListActivity("FileBrowser", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      mode(mode),
      basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    rebuildRowItems();
    return;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    rebuildRowItems();
    return;
  }

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    const bool isDirectory = file.isDirectory();
    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||
        strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      continue;
    }

    if (isDirectory) {
      files.emplace_back(std::string(fileNameBuffer.get()) + "/");
    } else {
      std::string_view filename{fileNameBuffer.get()};
      if (mode == Mode::PickFirmware) {
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename)) {
        files.emplace_back(filename);
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
  rebuildRowItems();
}

void FileBrowserActivity::rebuildRowItems() {
  rowsUseFileIcons = UITheme::getInstance().getTheme().showsFileIcons();
  rowNames.resize(files.size());
  rowExtensions.resize(files.size());
  rowItems.clear();
  rowItems.reserve(files.size());
  for (size_t i = 0; i < files.size(); i++) {
    rowNames[i] = getFileName(files[i]);
    rowExtensions[i] = getFileExtension(files[i]);
    fui::ListItem item;
    item.label = rowNames[i].c_str();
    if (!rowExtensions[i].empty()) item.value = rowExtensions[i].c_str();
    item.icon = listIconFor(UITheme::getFileIcon(files[i]));
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }

  struct PrewarmCtx {
    const std::vector<std::string>* names;
    const std::string* path;
  } prewarmCtx{&rowNames, &basepath};
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        const auto* c = static_cast<const PrewarmCtx*>(ctx);
        return i < c->names->size() ? (*c->names)[i].c_str() : c->path->c_str();
      },
      &prewarmCtx, static_cast<uint32_t>(rowNames.size()) + 1);
}

void FileBrowserActivity::onEnter() {
  UiListActivity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    nav.selected = static_cast<int>(findEntry(fileName));
  } else {
    loadFiles();
  }
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  rowNames.clear();
  rowExtensions.clear();
  rowItems.clear();
  fileNameBuffer.reset();
}

bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    clearBookCache(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') entryPath += "/";
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearBookCache(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}

void FileBrowserActivity::activateIndex(const int index) {
  (void)index;
  app.clearTapFlash();
  activateSelected();
}

void FileBrowserActivity::onRowLongPress(const int index) {
  (void)index;
  if (files.empty()) return;
  if (nav.selected < 0 || nav.selected >= listCount()) return;
  if (mode != Mode::Books) return;

  app.clearTapFlash();

  const std::string& entry = files[nav.selected];
  const bool isDirectory = (entry.back() == '/');
  // For keyboard pre-fill: bare name without trailing slash for dirs, full "name.ext" for files.
  const std::string currentName = isDirectory ? entry.substr(0, entry.length() - 1) : std::string(entry);

  std::string cleanBasePath = basepath;
  if (cleanBasePath.back() != '/') cleanBasePath += "/";
  const std::string fullOldPath = cleanBasePath + currentName;

  // Capture all context for the two action branches (rename / delete).
  // Using shared_ptr so both lambdas can hold a reference without copying
  // the strings multiple times on the small ESP32 heap.
  struct Ctx {
    std::string fullOldPath;
    std::string cleanBasePath;
    std::string currentName;
    bool isDirectory;
  };
  auto ctx = std::make_shared<Ctx>(Ctx{fullOldPath, cleanBasePath, currentName, isDirectory});

  // Capture 'this' and ctx by value (ctx is shared_ptr so the copy is cheap).
  auto launchRename = [this, ctx]() {
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput,
                                               tr(STR_RENAME), ctx->currentName, /*maxLength=*/255),
        [this, ctx](const ActivityResult& res) {
          if (res.isCancelled) return;

          const auto* sr = std::get_if<KeyboardResult>(&res.data);
          if (!sr || sr->text.empty()) return;

          std::string newName = sr->text;
          // Strip any trailing slash the user may have typed for directories.
          if (!newName.empty() && newName.back() == '/') newName.pop_back();
          if (newName.empty() || newName == ctx->currentName) return;

          const std::string fullNewPath = ctx->cleanBasePath + newName;

          LOG_DBG("FileBrowser", "Renaming: %s -> %s", ctx->fullOldPath.c_str(), fullNewPath.c_str());
          if (!Storage.rename(ctx->fullOldPath.c_str(), fullNewPath.c_str())) {
            LOG_ERR("FileBrowser", "Rename failed");
            return;
          }

          // After a successful rename, update the home-screen RecentBooks title
          // so the new filename appears immediately without reopening the book.
          // We seed the title from the new filename; real metadata (epub title or
          // a network lookup) will overwrite this the next time the book is opened.
          if (!ctx->isDirectory) {
            const std::string newTitle = titleFromFilename(newName);
            // updateBook(path, title, author, coverBmpPath):
            // Passing "" for author/cover preserves any cached values on some
            // implementations; if your RecentBooksStore clears them on "", use
            // existing book fields from RECENT_BOOKS.getBooks() instead.
            RECENT_BOOKS.updateBook(ctx->fullOldPath, newTitle, "", "");
            clearBookCache(ctx->fullOldPath);
          }

          {
            RenderLock lock(*this);
            loadFiles();
            const std::string newEntry = newName + (ctx->isDirectory ? "/" : "");
            nav.selected = static_cast<int>(findEntry(newEntry));
            nav.follow(listCount());
          }
          requestUpdate(true);
        });
  };

  auto launchDelete = [this, ctx, entry]() {
    std::string heading = tr(STR_DELETE) + std::string("? ");
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry),
        [this, ctx](const ActivityResult& res) {
          if (res.isCancelled) return;
          LOG_DBG("FileBrowser", "Deleting: %s", ctx->fullOldPath.c_str());
          if (removeDirFile(ctx->fullOldPath)) {
            RenderLock lock(*this);
            loadFiles();
            if (files.empty()) {
              nav.selected = 0;
            } else if (nav.selected >= listCount()) {
              nav.selected = listCount() - 1;
            }
            nav.follow(listCount());
            lock.unlock();
            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Delete failed: %s", ctx->fullOldPath.c_str());
          }
        });
  };

  // Show a two-option context menu: Edit Name / Delete.
  startActivityForResult(
      std::make_unique<ContextMenuActivity>(
          renderer, mappedInput,
          currentName,                                    // heading = the filename being acted on
          std::vector<std::string>{tr(STR_RENAME), tr(STR_DELETE)},
          [launchRename, launchDelete](int choice) {
            if (choice == 0) launchRename();
            else if (choice == 1) launchDelete();
            // choice == -1 (cancelled): do nothing
          }),
      [](const ActivityResult&) {});  // result handled by the onChoice callback above
}

void FileBrowserActivity::activateSelected(const bool forceDelete) {
  if (files.empty()) return;
  if (nav.selected < 0 || nav.selected >= listCount()) return;

  const std::string& entry = files[nav.selected];
  bool isDirectory = (entry.back() == '/');

  if (mode == Mode::PickFirmware && !isDirectory) {
    std::string cleanBasePath = basepath;
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    ActivityResult res{FilePathResult{cleanBasePath + entry}};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mode == Mode::Books && (forceDelete || mappedInput.getHeldTime() >= GO_HOME_MS)) {
    // Hardware-button long-press delete path (kept for physical-button users).
    std::string cleanBasePath = basepath;
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    const std::string fullPath = cleanBasePath + entry;

    auto handler = [this, fullPath](const ActivityResult& res) {
      if (!res.isCancelled) {
        LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
        if (removeDirFile(fullPath)) {
          LOG_DBG("FileBrowser", "Deleted successfully");
          {
            RenderLock lock(*this);
            loadFiles();
            if (files.empty()) {
              nav.selected = 0;
            } else if (nav.selected >= listCount()) {
              nav.selected = listCount() - 1;
            }
            nav.follow(listCount());
          }
          requestUpdate(true);
        } else {
          LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
        }
      }
    };

    std::string heading = tr(STR_DELETE) + std::string("? ");
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
    return;
  } else {
    RenderLock lock(*this);
    if (basepath.back() != '/') basepath += "/";

    if (isDirectory) {
      basepath += entry.substr(0, entry.length() - 1);
      loadFiles();
      nav.selected = 0;
      nav.top = 0;
      lock.unlock();
      requestUpdate();
    } else {
      const std::string fullPath = basepath + entry;
      lock.unlock();
      onSelectBook(fullPath);
    }
  }
  return;
}

bool FileBrowserActivity::handleCustomInput() {
  if (mode == Mode::Books && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && basepath != "/") {
    {
      RenderLock lock(*this);
      basepath = "/";
      loadFiles();
      nav.selected = 0;
      nav.top = 0;
    }
    requestUpdate();
    return true;
  }

  return false;
}

bool FileBrowserActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        {
          RenderLock lock(*this);
          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadFiles();

          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          nav.selected = static_cast<int>(findEntry(dirName));
          nav.top = 0;
          nav.follow(listCount());
        }

        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
    return true;
  }

  return false;
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(const std::string& filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  {
    const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(pathLineHeight + metrics.verticalSpacing));
    screen.target().fill(fui::Rect{band.x, band.y, band.width, 3}, fui::Paint::solid(fui::Color::Black));
    const int pathY =
        band.y + metrics.verticalSpacing / 2 + (band.height - metrics.verticalSpacing / 2 - pathLineHeight) / 2;
    const int pathMaxWidth = band.width - metrics.contentSidePadding * 2;
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, band.x + metrics.contentSidePadding, pathY, pathDisplay);
  }

  if (files.empty()) {
    screen.centeredText(mode == Mode::PickFirmware ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND),
                        screen.theme().bodyText);
    return;
  }

  if (rowsUseFileIcons != UITheme::getInstance().getTheme().showsFileIcons()) {
    rebuildRowItems();
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  // Tap opens/navigates; long-press shows Edit Name / Delete menu.
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;
  fui::TextStyle label = screen.theme().smallText;
  label.maxLines = 2;
  props.labelText = label;
  props.balanceWrappedLabelWithValue = false;
  props.partialTrailingRow = true;
  syncListViewport(screen, props);
  screen.list(props);
}

void FileBrowserActivity::drawChrome() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());
}

void FileBrowserActivity::drawFooter() {
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && nav.selected >= 0 &&
                                     nav.selected < listCount() && files[nav.selected].back() != '/';
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                            files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}