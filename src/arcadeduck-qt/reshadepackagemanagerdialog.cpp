// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "reshadepackagemanagerdialog.h"

#include "qthost.h"
#include "qtprogresscallback.h"
#include "qtutils.h"

#include "core/settings.h"
#include "util/http_downloader.h"
#include "util/postprocessing_shader_fx.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/minizip_helpers.h"
#include "common/path.h"
#include "common/string_util.h"

#include "unzip.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_set>

#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

Log_SetChannel(ReShadePackageManager);

namespace {

static constexpr const char* PACKAGE_CATALOG_URL =
  "https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini";
static constexpr u32 HTTP_POLL_INTERVAL = 10;
static constexpr u64 MAX_PACKAGE_FILE_SIZE = 256ull * 1024ull * 1024ull;
static constexpr u64 MAX_PACKAGE_EXTRACTED_SIZE = 768ull * 1024ull * 1024ull;

static std::string_view Trim(std::string_view value)
{
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

static bool EqualNoCase(std::string_view lhs, std::string_view rhs)
{
  return StringUtil::EqualNoCase(lhs, rhs);
}

static bool EndsWithNoCase(std::string_view value, std::string_view suffix)
{
  if (suffix.size() > value.size())
    return false;
  return EqualNoCase(value.substr(value.size() - suffix.size()), suffix);
}

static std::vector<std::string> SplitCSV(std::string_view value)
{
  std::vector<std::string> result;
  while (!value.empty())
  {
    const size_t comma = value.find(',');
    const std::string_view part = Trim(value.substr(0, comma));
    if (!part.empty())
      result.emplace_back(part);

    if (comma == std::string_view::npos)
      break;
    value.remove_prefix(comma + 1);
  }
  return result;
}

static bool ContainsNoCase(const std::vector<std::string>& values, std::string_view value)
{
  return std::any_of(values.begin(), values.end(), [value](const std::string& item) { return EqualNoCase(item, value); });
}

static std::string NormalizeSlashPath(std::string_view path)
{
  std::string result(path);
  StringUtil::ReplaceAll(&result, '\\', '/');

  while (result.size() >= 2 && result[0] == '.' && result[1] == '/')
    result.erase(0, 2);
  while (!result.empty() && result.front() == '/')
    result.erase(result.begin());

  return result;
}

static bool IsSafeRelativePath(std::string_view path)
{
  if (path.empty() || path.front() == '/' || path.front() == '\\')
    return false;

  const std::string normalized = NormalizeSlashPath(path);
  if (normalized.empty() || normalized.find(':') != std::string::npos)
    return false;

  size_t offset = 0;
  while (offset <= normalized.size())
  {
    const size_t slash = normalized.find('/', offset);
    const std::string_view component =
      std::string_view(normalized).substr(offset, slash == std::string::npos ? std::string::npos : slash - offset);
    if (component == "..")
      return false;

    if (slash == std::string::npos)
      break;
    offset = slash + 1;
  }

  return true;
}

static std::optional<std::string> NormalizeArchiveMember(std::string_view path)
{
  std::string normalized = NormalizeSlashPath(path);
  if (!IsSafeRelativePath(normalized))
    return std::nullopt;

  std::string compact;
  compact.reserve(normalized.size());
  size_t offset = 0;
  while (offset <= normalized.size())
  {
    const size_t slash = normalized.find('/', offset);
    const std::string_view component =
      std::string_view(normalized).substr(offset, slash == std::string::npos ? std::string::npos : slash - offset);

    if (!component.empty() && component != ".")
    {
      if (!compact.empty())
        compact.push_back('/');
      compact.append(component);
    }

    if (slash == std::string::npos)
      break;
    offset = slash + 1;
  }

  return compact.empty() ? std::nullopt : std::optional<std::string>(std::move(compact));
}

static std::string ToNativePath(std::string path)
{
#ifdef _WIN32
  StringUtil::ReplaceAll(&path, '/', '\\');
#else
  StringUtil::ReplaceAll(&path, '\\', '/');
#endif
  return path;
}

static std::string GetBaseName(std::string_view path)
{
  const size_t slash = path.find_last_of("/\\");
  return std::string((slash == std::string_view::npos) ? path : path.substr(slash + 1));
}

static std::string GetDirectoryName(std::string_view path)
{
  const size_t slash = path.find_last_of("/\\");
  return (slash == std::string_view::npos) ? std::string() : std::string(path.substr(0, slash));
}

static std::string SanitizePackageName(std::string_view name)
{
  std::string result;
  result.reserve(name.size());

  bool previous_separator = false;
  for (const char ch : name)
  {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch))
    {
      result.push_back(static_cast<char>(std::tolower(uch)));
      previous_separator = false;
    }
    else if (!previous_separator && !result.empty())
    {
      result.push_back('-');
      previous_separator = true;
    }
  }

  while (!result.empty() && result.back() == '-')
    result.pop_back();

  return result.empty() ? "package" : result;
}

static std::string GetPathAfterDirectory(std::string_view path, std::string_view directory)
{
  const std::string normalized = NormalizeSlashPath(path);
  size_t offset = 0;

  while (offset <= normalized.size())
  {
    const size_t slash = normalized.find('/', offset);
    const std::string_view component =
      std::string_view(normalized).substr(offset, slash == std::string::npos ? std::string::npos : slash - offset);

    if (EqualNoCase(component, directory))
    {
      if (slash == std::string::npos)
        return {};
      return normalized.substr(slash + 1);
    }

    if (slash == std::string::npos)
      break;
    offset = slash + 1;
  }

  return {};
}

static std::optional<std::string> FindNamedDirectoryRoot(const std::vector<std::string>& members,
                                                         std::string_view directory)
{
  std::optional<std::string> best;

  for (const std::string& member : members)
  {
    size_t offset = 0;
    while (offset <= member.size())
    {
      const size_t slash = member.find('/', offset);
      const std::string_view component =
        std::string_view(member).substr(offset, slash == std::string::npos ? std::string::npos : slash - offset);

      if (EqualNoCase(component, directory))
      {
        const std::string candidate =
          member.substr(0, slash == std::string::npos ? member.size() : slash);
        if (!best.has_value() || candidate.size() < best->size())
          best = candidate;
        break;
      }

      if (slash == std::string::npos)
        break;
      offset = slash + 1;
    }
  }

  return best;
}

static std::optional<std::string> FindFallbackRoot(const std::vector<std::string>& members, bool textures)
{
  std::optional<std::string> best;

  for (const std::string& member : members)
  {
    const bool candidate =
      textures ? (EndsWithNoCase(member, ".png") || EndsWithNoCase(member, ".jpg") || EndsWithNoCase(member, ".jpeg") ||
                  EndsWithNoCase(member, ".dds") || EndsWithNoCase(member, ".bmp") || EndsWithNoCase(member, ".tga"))
               : EndsWithNoCase(member, ".fx");
    if (!candidate)
      continue;

    const std::string directory = GetDirectoryName(member);
    if (!best.has_value() || directory.size() < best->size())
      best = directory;
  }

  return best;
}

static std::optional<std::string> GetRelativeToRoot(std::string_view member, std::string_view root)
{
  if (root.empty())
    return std::string(member);

  if (member.size() <= root.size() || !EqualNoCase(member.substr(0, root.size()), root) || member[root.size()] != '/')
    return std::nullopt;

  return std::string(member.substr(root.size() + 1));
}

static std::string JoinRelativePath(std::string_view first, std::string_view second)
{
  if (first.empty())
    return NormalizeSlashPath(second);
  if (second.empty())
    return NormalizeSlashPath(first);

  std::string result = NormalizeSlashPath(first);
  if (!result.empty() && result.back() != '/')
    result.push_back('/');
  result.append(NormalizeSlashPath(second));
  return result;
}

} // namespace

ReShadePackageManagerDialog::ReShadePackageManagerDialog(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("ReShade Effect Packages"));
  setMinimumSize(900, 600);
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_http = HTTPDownloader::Create(Host::GetHTTPUserAgent());

  QVBoxLayout* main_layout = new QVBoxLayout(this);

  QLabel* warning = new QLabel(
    tr("Packages are downloaded from ReShade's official EffectPackages.ini catalog. "
       "ArcadeDuck can automatically preflight each effect with its actual ReShadeFX loader and "
       "disable effects that use unsupported constructs. A successful preflight does not guarantee "
       "identical behavior on every graphics backend."),
    this);
  warning->setWordWrap(true);
  main_layout->addWidget(warning);

  QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
  main_layout->addWidget(splitter, 1);

  m_package_list = new QTreeWidget(splitter);
  m_package_list->setHeaderLabels({tr("Package"), tr("Status")});
  m_package_list->setRootIsDecorated(false);
  m_package_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_package_list->setAlternatingRowColors(true);
  m_package_list->header()->setStretchLastSection(false);
  m_package_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_package_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

  QWidget* details_widget = new QWidget(splitter);
  QVBoxLayout* details_layout = new QVBoxLayout(details_widget);

  m_description = new QLabel(details_widget);
  m_description->setWordWrap(true);
  m_description->setTextInteractionFlags(Qt::TextSelectableByMouse);
  details_layout->addWidget(m_description);

  QLabel* effects_label = new QLabel(tr("Effects to install:"), details_widget);
  details_layout->addWidget(effects_label);

  m_effect_list = new QListWidget(details_widget);
  m_effect_list->setAlternatingRowColors(true);
  details_layout->addWidget(m_effect_list, 1);

  m_status = new QLabel(details_widget);
  m_status->setWordWrap(true);
  details_layout->addWidget(m_status);

  splitter->addWidget(m_package_list);
  splitter->addWidget(details_widget);
  splitter->setStretchFactor(0, 2);
  splitter->setStretchFactor(1, 3);

  QHBoxLayout* action_layout = new QHBoxLayout();
  QPushButton* refresh_button = new QPushButton(tr("Refresh Catalog"), this);
  m_check_button = new QPushButton(tr("Check Compatibility"), this);
  m_repository_button = new QPushButton(tr("Repository"), this);
  m_install_button = new QPushButton(tr("Install"), this);
  m_remove_button = new QPushButton(tr("Remove"), this);

  action_layout->addWidget(refresh_button);
  action_layout->addWidget(m_check_button);
  action_layout->addWidget(m_repository_button);
  action_layout->addStretch(1);
  action_layout->addWidget(m_remove_button);
  action_layout->addWidget(m_install_button);
  main_layout->addLayout(action_layout);

  QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  main_layout->addWidget(buttons);

  connect(refresh_button, &QPushButton::clicked, this, [this]() { refreshCatalog(); });
  connect(m_check_button, &QPushButton::clicked, this, [this]() { checkSelectedPackageCompatibility(); });
  connect(m_repository_button, &QPushButton::clicked, this, [this]() { openSelectedRepository(); });
  connect(m_install_button, &QPushButton::clicked, this, [this]() { installOrUpdateSelectedPackage(); });
  connect(m_remove_button, &QPushButton::clicked, this, [this]() { removeSelectedPackage(); });
  connect(m_package_list, &QTreeWidget::itemSelectionChanged, this, [this]() { updateSelectionDetails(); });

  refreshInstalledPackages();
  rebuildPackageList();
  updateSelectionDetails();

  if (!m_http)
  {
    m_status->setText(tr("HTTP support is unavailable in this build."));
    refresh_button->setEnabled(false);
    m_check_button->setEnabled(false);
  }
  else
  {
    QTimer::singleShot(0, this, [this]() { refreshCatalog(); });
  }
}

ReShadePackageManagerDialog::~ReShadePackageManagerDialog() = default;

bool ReShadePackageManagerDialog::downloadUrl(const std::string& url, const QString& status_text, std::vector<u8>* output_data)
{
  if (!m_http)
    return false;

  std::optional<s32> result;
  std::vector<u8> response;

  QtModalProgressCallback progress(this);
  progress.SetTitle(tr("ReShade Effect Packages").toUtf8().constData());
  progress.SetStatusText(status_text.toUtf8().constData());
  progress.SetCancellable(true);

  m_http->CreateRequest(
    url,
    [&result, &response](s32 status_code, const std::string&, std::vector<u8> body) {
      result = status_code;
      if (status_code == HTTPDownloader::HTTP_STATUS_OK)
        response = std::move(body);
    },
    &progress);

  while (m_http->HasAnyRequests())
  {
    QApplication::processEvents(QEventLoop::AllEvents, HTTP_POLL_INTERVAL);
    m_http->PollRequests();
  }

  if (!result.has_value() || result.value() == HTTPDownloader::HTTP_STATUS_CANCELLED)
    return false;

  if (result.value() != HTTPDownloader::HTTP_STATUS_OK)
  {
    QMessageBox::critical(this, tr("Download Failed"),
                          tr("The download failed with HTTP status %1.").arg(result.value()));
    return false;
  }

  if (response.empty())
  {
    QMessageBox::critical(this, tr("Download Failed"), tr("The server returned an empty response."));
    return false;
  }

  *output_data = std::move(response);
  return true;
}

bool ReShadePackageManagerDialog::parseCatalog(std::string_view text, std::vector<EffectPackage>* packages,
                                               std::string* error) const
{
  packages->clear();

  EffectPackage current;
  bool have_section = false;

  auto finish_section = [&]() {
    if (!have_section)
      return;

    if (!current.name.empty() && !current.download_url.empty())
    {
      current.effect_files.erase(
        std::remove_if(current.effect_files.begin(), current.effect_files.end(),
                       [&current](const std::string& effect) {
                         return ContainsNoCase(current.deny_effect_files, effect);
                       }),
        current.effect_files.end());
      packages->push_back(std::move(current));
    }

    current = {};
    have_section = false;
  };

  size_t offset = 0;
  while (offset <= text.size())
  {
    const size_t newline = text.find('\n', offset);
    std::string_view line =
      text.substr(offset, newline == std::string_view::npos ? std::string_view::npos : newline - offset);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    line = Trim(line);

    if (!line.empty() && line.front() != ';' && line.front() != '#')
    {
      if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
      {
        finish_section();
        current.section = std::string(Trim(line.substr(1, line.size() - 2)));
        have_section = true;
      }
      else if (have_section)
      {
        const size_t equals = line.find('=');
        if (equals != std::string_view::npos)
        {
          const std::string_view key = Trim(line.substr(0, equals));
          const std::string_view value = Trim(line.substr(equals + 1));

          if (EqualNoCase(key, "PackageName"))
            current.name = std::string(value);
          else if (EqualNoCase(key, "PackageDescription"))
            current.description = std::string(value);
          else if (EqualNoCase(key, "InstallPath"))
            current.install_path = std::string(value);
          else if (EqualNoCase(key, "TextureInstallPath"))
            current.texture_install_path = std::string(value);
          else if (EqualNoCase(key, "DownloadUrl"))
            current.download_url = std::string(value);
          else if (EqualNoCase(key, "RepositoryUrl"))
            current.repository_url = std::string(value);
          else if (EqualNoCase(key, "EffectFiles"))
            current.effect_files = SplitCSV(value);
          else if (EqualNoCase(key, "DenyEffectFiles"))
            current.deny_effect_files = SplitCSV(value);
        }
      }
    }

    if (newline == std::string_view::npos)
      break;
    offset = newline + 1;
  }

  finish_section();

  if (packages->empty())
  {
    if (error)
      *error = "No effect packages were found in the downloaded catalog.";
    return false;
  }

  std::sort(packages->begin(), packages->end(), [](const EffectPackage& lhs, const EffectPackage& rhs) {
    return StringUtil::Strcasecmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
  });
  return true;
}

bool ReShadePackageManagerDialog::readInstalledManifest(const std::string& path, InstalledPackage* package) const
{
  const std::optional<std::string> content = FileSystem::ReadFileToString(path.c_str());
  if (!content.has_value())
    return false;

  InstalledPackage result;
  result.manifest_path = path;

  size_t offset = 0;
  while (offset <= content->size())
  {
    const size_t newline = content->find('\n', offset);
    std::string_view line =
      std::string_view(*content).substr(offset, newline == std::string::npos ? std::string_view::npos : newline - offset);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    line = Trim(line);

    const size_t equals = line.find('=');
    if (equals != std::string_view::npos)
    {
      const std::string_view key = Trim(line.substr(0, equals));
      const std::string_view value = Trim(line.substr(equals + 1));

      if (EqualNoCase(key, "PackageName"))
        result.package_name = std::string(value);
      else if (EqualNoCase(key, "DownloadUrl"))
        result.download_url = std::string(value);
      else if (EqualNoCase(key, "RepositoryUrl"))
        result.repository_url = std::string(value);
      else if (EqualNoCase(key, "SelectedEffect"))
        result.selected_effects.emplace_back(value);
      else if (EqualNoCase(key, "File") && IsSafeRelativePath(value))
        result.files.emplace_back(NormalizeSlashPath(value));
    }

    if (newline == std::string::npos)
      break;
    offset = newline + 1;
  }

  if (result.package_name.empty() || result.download_url.empty())
    return false;

  *package = std::move(result);
  return true;
}

void ReShadePackageManagerDialog::refreshInstalledPackages()
{
  m_installed_packages.clear();

  const std::string package_dir =
    Path::Combine(EmuFolders::Shaders, "reshade" FS_OSPATH_SEPARATOR_STR ".packages");
  FileSystem::EnsureDirectoryExists(package_dir.c_str(), true);

  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(package_dir.c_str(), "*.manifest",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS, &results);

  for (const FILESYSTEM_FIND_DATA& result : results)
  {
    InstalledPackage package;
    if (readInstalledManifest(Path::Combine(package_dir, result.FileName), &package))
      m_installed_packages.push_back(std::move(package));
  }
}

s32 ReShadePackageManagerDialog::findInstalledPackage(const EffectPackage& package) const
{
  for (size_t i = 0; i < m_installed_packages.size(); i++)
  {
    if (EqualNoCase(m_installed_packages[i].download_url, package.download_url))
      return static_cast<s32>(i);
  }

  for (size_t i = 0; i < m_installed_packages.size(); i++)
  {
    if (EqualNoCase(m_installed_packages[i].package_name, package.name))
      return static_cast<s32>(i);
  }

  return -1;
}

void ReShadePackageManagerDialog::refreshCatalog()
{
  m_cached_package_url.clear();
  m_cached_package_archive.clear();

  std::vector<u8> catalog_data;
  if (!downloadUrl(PACKAGE_CATALOG_URL, tr("Downloading the official ReShade effect package catalog..."), &catalog_data))
    return;

  std::vector<EffectPackage> packages;
  std::string error;
  const std::string_view text(reinterpret_cast<const char*>(catalog_data.data()), catalog_data.size());
  if (!parseCatalog(text, &packages, &error))
  {
    QMessageBox::critical(this, tr("Catalog Error"), QString::fromStdString(error));
    return;
  }

  refreshInstalledPackages();

  for (const InstalledPackage& installed : m_installed_packages)
  {
    const bool present =
      std::any_of(packages.begin(), packages.end(), [&installed](const EffectPackage& package) {
        return EqualNoCase(package.download_url, installed.download_url) ||
               EqualNoCase(package.name, installed.package_name);
      });

    if (!present)
    {
      EffectPackage orphan;
      orphan.name = installed.package_name;
      orphan.description = "This installed package is no longer present in the current official catalog.";
      orphan.download_url = installed.download_url;
      orphan.repository_url = installed.repository_url;
      orphan.from_catalog = false;
      packages.push_back(std::move(orphan));
    }
  }

  std::sort(packages.begin(), packages.end(), [](const EffectPackage& lhs, const EffectPackage& rhs) {
    return StringUtil::Strcasecmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
  });

  m_packages = std::move(packages);
  rebuildPackageList();
  m_status->setText(tr("Loaded %1 packages from the official ReShade catalog.")
                      .arg(static_cast<qulonglong>(m_packages.size())));
}

void ReShadePackageManagerDialog::rebuildPackageList(std::string_view preserve_download_url)
{
  if (preserve_download_url.empty())
  {
    if (const std::optional<size_t> selected = getSelectedPackageIndex(); selected.has_value())
      preserve_download_url = m_packages[selected.value()].download_url;
  }

  m_package_list->clear();

  for (size_t i = 0; i < m_packages.size(); i++)
  {
    const EffectPackage& package = m_packages[i];
    QTreeWidgetItem* item = new QTreeWidgetItem(m_package_list);
    item->setText(0, QString::fromStdString(package.name));

    if (package.compatibility_scanned)
    {
      size_t compatible = 0;
      size_t unsupported = 0;
      size_t unknown = 0;
      for (const EffectCompatibility& result : package.compatibility)
      {
        compatible += (result.status == EffectCompatibilityStatus::Compatible) ? 1 : 0;
        unsupported += (result.status == EffectCompatibilityStatus::Unsupported) ? 1 : 0;
        unknown += (result.status == EffectCompatibilityStatus::Unknown) ? 1 : 0;
      }

      if (compatible == 0)
        item->setText(1, tr("Unsupported"));
      else if (unsupported == 0 && unknown == 0)
        item->setText(1, tr("Compatible"));
      else
        item->setText(1, tr("%1 compatible").arg(static_cast<qulonglong>(compatible)));
    }
    else
    {
      item->setText(1, findInstalledPackage(package) >= 0 ? tr("Installed") : tr("Available"));
    }

    item->setData(0, Qt::UserRole, static_cast<qulonglong>(i));

    if (!preserve_download_url.empty() && EqualNoCase(package.download_url, preserve_download_url))
      m_package_list->setCurrentItem(item);
  }

  if (!m_package_list->currentItem() && m_package_list->topLevelItemCount() > 0)
    m_package_list->setCurrentItem(m_package_list->topLevelItem(0));

  updateSelectionDetails();
}

std::optional<size_t> ReShadePackageManagerDialog::getSelectedPackageIndex() const
{
  const QList<QTreeWidgetItem*> selected = m_package_list->selectedItems();
  if (selected.empty())
    return std::nullopt;

  const qulonglong index = selected.first()->data(0, Qt::UserRole).toULongLong();
  if (index >= m_packages.size())
    return std::nullopt;

  return static_cast<size_t>(index);
}

void ReShadePackageManagerDialog::updateSelectionDetails()
{
  m_effect_list->clear();

  const std::optional<size_t> selected = getSelectedPackageIndex();
  if (!selected.has_value())
  {
    m_description->clear();
    m_check_button->setEnabled(false);
    m_repository_button->setEnabled(false);
    m_install_button->setEnabled(false);
    m_remove_button->setEnabled(false);
    return;
  }

  const EffectPackage& package = m_packages[selected.value()];
  const s32 installed_index = findInstalledPackage(package);
  const InstalledPackage* installed =
    (installed_index >= 0) ? &m_installed_packages[static_cast<size_t>(installed_index)] : nullptr;

  QString description = QString::fromStdString(package.description);
  if (!package.from_catalog)
    description.append(tr("\n\nThis package is installed locally but is no longer listed in the current catalog."));
  m_description->setText(description);

  size_t compatible_count = 0;
  size_t unsupported_count = 0;
  size_t unknown_count = 0;

  for (const std::string& effect : package.effect_files)
  {
    const EffectCompatibility* compatibility = findEffectCompatibility(package, effect);
    QString item_text = QString::fromStdString(effect);

    if (compatibility)
    {
      switch (compatibility->status)
      {
        case EffectCompatibilityStatus::Compatible:
          item_text.append(tr("  [Compatible]"));
          compatible_count++;
          break;

        case EffectCompatibilityStatus::Unsupported:
          item_text.append(tr("  [Unsupported]"));
          unsupported_count++;
          break;

        case EffectCompatibilityStatus::Unknown:
          item_text.append(tr("  [Unknown]"));
          unknown_count++;
          break;

        case EffectCompatibilityStatus::Unchecked:
        default:
          break;
      }
    }

    QListWidgetItem* item = new QListWidgetItem(item_text, m_effect_list);
    item->setData(Qt::UserRole, QString::fromStdString(effect));

    const bool selectable =
      !compatibility || compatibility->status == EffectCompatibilityStatus::Unchecked ||
      compatibility->status == EffectCompatibilityStatus::Compatible;
    if (selectable)
    {
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
      const bool checked =
        !installed || installed->selected_effects.empty() || ContainsNoCase(installed->selected_effects, effect);
      item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    else
    {
      Qt::ItemFlags flags = item->flags();
      flags &= ~Qt::ItemIsEnabled;
      flags &= ~Qt::ItemIsUserCheckable;
      item->setFlags(flags);
      if (!compatibility->reason.empty())
        item->setToolTip(QString::fromStdString(compatibility->reason));
    }
  }

  if (package.effect_files.empty())
  {
    QListWidgetItem* item = new QListWidgetItem(
      package.from_catalog ? tr("This package does not provide an individual effect list; all shader files will be installed.")
                           : tr("Effect selection is unavailable for this installed package."),
      m_effect_list);
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
  }

  m_check_button->setEnabled(package.from_catalog && !package.download_url.empty());
  m_repository_button->setEnabled(!package.repository_url.empty());
  m_install_button->setEnabled(package.from_catalog && !package.download_url.empty());
  m_install_button->setText(installed ? tr("Update / Reinstall") : tr("Install"));
  m_remove_button->setEnabled(installed != nullptr);

  if (package.compatibility_scanned)
  {
    m_status->setText(
      tr("Compatibility preflight: %1 compatible, %2 unsupported, %3 unknown. "
         "Unsupported and unknown effects are disabled.")
        .arg(static_cast<qulonglong>(compatible_count))
        .arg(static_cast<qulonglong>(unsupported_count))
        .arg(static_cast<qulonglong>(unknown_count)));
  }
  else if (package.from_catalog)
  {
    m_status->setText(tr("Compatibility has not been checked for this package yet."));
  }
}

const ReShadePackageManagerDialog::EffectCompatibility*
ReShadePackageManagerDialog::findEffectCompatibility(const EffectPackage& package, std::string_view effect) const
{
  for (const EffectCompatibility& result : package.compatibility)
  {
    if (EqualNoCase(result.effect, effect))
      return &result;
  }

  return nullptr;
}

bool ReShadePackageManagerDialog::scanPackageCompatibility(EffectPackage* package, std::string* error)
{
  std::vector<u8> archive;
  if (!downloadUrl(package->download_url,
                   tr("Checking compatibility for %1...").arg(QString::fromStdString(package->name)), &archive))
  {
    if (error)
      *error = "Download cancelled or failed.";
    return false;
  }

  unzFile zf = MinizipHelpers::OpenUnzMemoryFile(archive.data(), archive.size());
  if (!zf)
  {
    if (error)
      *error = "The downloaded file is not a readable ZIP archive.";
    return false;
  }

  struct MemberInfo
  {
    std::string original;
    std::string normalized;
    u64 size;
  };

  std::vector<std::string> members;
  std::vector<MemberInfo> member_info;

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    if (error)
      *error = "The downloaded ZIP archive is empty.";
    return false;
  }

  for (;;)
  {
    unz_file_info64 info = {};
    char name_buffer[4096] = {};
    if (unzGetCurrentFileInfo64(zf, &info, name_buffer, sizeof(name_buffer), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      if (error)
        *error = "Failed to read ZIP member information.";
      return false;
    }

    const std::string original(name_buffer);
    const std::optional<std::string> normalized = NormalizeArchiveMember(original);
    if (!normalized.has_value())
    {
      unzClose(zf);
      if (error)
        *error = "The package contains an unsafe archive path.";
      return false;
    }

    const bool is_directory = (!original.empty() && (original.back() == '/' || original.back() == '\\'));
    if (!is_directory)
    {
      if (info.uncompressed_size > MAX_PACKAGE_FILE_SIZE)
      {
        unzClose(zf);
        if (error)
          *error = "The package contains an unexpectedly large file.";
        return false;
      }

      members.push_back(normalized.value());
      member_info.push_back({original, normalized.value(), static_cast<u64>(info.uncompressed_size)});
    }

    const int next = unzGoToNextFile(zf);
    if (next == UNZ_END_OF_LIST_OF_FILE)
      break;
    if (next != UNZ_OK)
    {
      unzClose(zf);
      if (error)
        *error = "Failed while scanning the ZIP archive.";
      return false;
    }
  }

  const std::optional<std::string> shader_root = FindNamedDirectoryRoot(members, "Shaders");
  const std::optional<std::string> effective_shader_root =
    shader_root.has_value() ? shader_root : FindFallbackRoot(members, false);
  if (!effective_shader_root.has_value())
  {
    unzClose(zf);
    if (error)
      *error = "No ReShade FX shader directory could be found in the package.";
    return false;
  }

  std::vector<std::string> effects = package->effect_files;
  if (effects.empty())
  {
    for (const MemberInfo& member : member_info)
    {
      const std::optional<std::string> relative =
        GetRelativeToRoot(member.normalized, effective_shader_root.value());
      if (!relative.has_value() || !EndsWithNoCase(relative.value(), ".fx"))
        continue;

      const std::string basename = GetBaseName(relative.value());
      if (!ContainsNoCase(package->deny_effect_files, basename) && !ContainsNoCase(effects, basename))
        effects.push_back(basename);
    }

    std::sort(effects.begin(), effects.end(),
              [](const std::string& lhs, const std::string& rhs) {
                return StringUtil::Strcasecmp(lhs.c_str(), rhs.c_str()) < 0;
              });
  }

  const std::string packages_root =
    Path::Combine(EmuFolders::Shaders, "reshade" FS_OSPATH_SEPARATOR_STR ".packages");
  const std::string staging_root =
    Path::Combine(packages_root, ".compat-" + SanitizePackageName(package->name));

  FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
  if (!FileSystem::EnsureDirectoryExists(staging_root.c_str(), true))
  {
    unzClose(zf);
    if (error)
      *error = "Failed to create the compatibility staging directory.";
    return false;
  }

  u64 extracted_size = 0;
  for (const MemberInfo& member : member_info)
  {
    const std::optional<std::string> relative =
      GetRelativeToRoot(member.normalized, effective_shader_root.value());
    if (!relative.has_value())
      continue;

    extracted_size += member.size;
    if (extracted_size > MAX_PACKAGE_EXTRACTED_SIZE)
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "The shader tree expands beyond ArcadeDuck's safety limit.";
      return false;
    }

    if (unzLocateFile(zf, member.original.c_str(), 0) != UNZ_OK || unzOpenCurrentFile(zf) != UNZ_OK)
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "Failed to reopen a ZIP member during compatibility analysis.";
      return false;
    }

    std::vector<u8> contents(static_cast<size_t>(member.size));
    size_t read_offset = 0;
    while (read_offset < contents.size())
    {
      const unsigned request_size =
        static_cast<unsigned>(std::min<size_t>(contents.size() - read_offset, 64 * 1024));
      const int read = unzReadCurrentFile(zf, contents.data() + read_offset, request_size);
      if (read <= 0)
      {
        unzCloseCurrentFile(zf);
        unzClose(zf);
        FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
        if (error)
          *error = "Failed while extracting a shader file for compatibility analysis.";
        return false;
      }
      read_offset += static_cast<size_t>(read);
    }

    if (unzCloseCurrentFile(zf) != UNZ_OK)
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "ZIP CRC validation failed during compatibility analysis.";
      return false;
    }

    const std::string staging_path = Path::Combine(staging_root, ToNativePath(relative.value()));
    const std::string staging_dir(Path::GetDirectory(staging_path));
    if (!staging_dir.empty() && !FileSystem::EnsureDirectoryExists(staging_dir.c_str(), true))
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "Failed to create a compatibility staging subdirectory.";
      return false;
    }

    if (!FileSystem::WriteBinaryFile(staging_path.c_str(), contents.data(), contents.size()))
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "Failed to write a staged shader file.";
      return false;
    }
  }

  unzClose(zf);

  std::vector<EffectCompatibility> compatibility;
  compatibility.reserve(effects.size());

  for (const std::string& effect : effects)
  {
    EffectCompatibility result;
    result.effect = effect;
    result.status = EffectCompatibilityStatus::Unknown;

    std::optional<std::string> staged_effect;
    for (const MemberInfo& member : member_info)
    {
      const std::optional<std::string> relative =
        GetRelativeToRoot(member.normalized, effective_shader_root.value());
      if (!relative.has_value() || !EndsWithNoCase(relative.value(), ".fx"))
        continue;

      if (EqualNoCase(GetBaseName(relative.value()), effect))
      {
        staged_effect = Path::Combine(staging_root, ToNativePath(relative.value()));
        break;
      }
    }

    if (!staged_effect.has_value())
    {
      result.reason = "The effect listed by the catalog was not found in the downloaded package.";
      compatibility.push_back(std::move(result));
      continue;
    }

    Error shader_error;
    PostProcessing::ReShadeFXShader shader;
    shader.SetAdditionalIncludePath(staging_root);
    if (shader.LoadFromFile(effect, staged_effect.value(), true, &shader_error))
    {
      result.status = EffectCompatibilityStatus::Compatible;
      result.reason = "Passes ArcadeDuck's ReShadeFX parser and runtime-feature preflight.";
    }
    else
    {
      result.status = EffectCompatibilityStatus::Unsupported;
      result.reason = shader_error.GetDescription();
      if (result.reason.empty())
        result.reason = "ArcadeDuck's ReShadeFX loader rejected this effect.";
    }

    compatibility.push_back(std::move(result));
  }

  FileSystem::RecursiveDeleteDirectory(staging_root.c_str());

  package->effect_files = std::move(effects);
  package->compatibility = std::move(compatibility);
  package->compatibility_scanned = true;

  m_cached_package_url = package->download_url;
  m_cached_package_archive = std::move(archive);
  return true;
}

void ReShadePackageManagerDialog::checkSelectedPackageCompatibility()
{
  const std::optional<size_t> selected = getSelectedPackageIndex();
  if (!selected.has_value())
    return;

  EffectPackage& package = m_packages[selected.value()];
  if (!package.from_catalog)
    return;

  std::string error;
  if (!scanPackageCompatibility(&package, &error))
  {
    if (error != "Download cancelled or failed.")
      QMessageBox::critical(this, tr("Compatibility Check Failed"), QString::fromStdString(error));
    return;
  }

  const std::string preserve_url = package.download_url;
  rebuildPackageList(preserve_url);
}

std::vector<std::string> ReShadePackageManagerDialog::getCheckedEffects() const
{
  std::vector<std::string> result;
  for (int i = 0; i < m_effect_list->count(); i++)
  {
    const QListWidgetItem* item = m_effect_list->item(i);
    if ((item->flags() & Qt::ItemIsUserCheckable) && item->checkState() == Qt::Checked)
      result.push_back(item->data(Qt::UserRole).toString().toStdString());
  }
  return result;
}

void ReShadePackageManagerDialog::openSelectedRepository()
{
  const std::optional<size_t> selected = getSelectedPackageIndex();
  if (!selected.has_value() || m_packages[selected.value()].repository_url.empty())
    return;

  QtUtils::OpenURL(this, m_packages[selected.value()].repository_url.c_str());
}

bool ReShadePackageManagerDialog::installPackage(const EffectPackage& package,
                                                 const std::vector<std::string>& selected_effects, std::string* error)
{
  std::vector<u8> archive;
  if (EqualNoCase(m_cached_package_url, package.download_url) && !m_cached_package_archive.empty())
  {
    archive = std::move(m_cached_package_archive);
    m_cached_package_url.clear();
  }
  else if (!downloadUrl(package.download_url, tr("Downloading %1...").arg(QString::fromStdString(package.name)),
                        &archive))
  {
    if (error)
      *error = "Download cancelled or failed.";
    return false;
  }

  unzFile zf = MinizipHelpers::OpenUnzMemoryFile(archive.data(), archive.size());
  if (!zf)
  {
    if (error)
      *error = "The downloaded file is not a readable ZIP archive.";
    return false;
  }

  std::vector<std::string> members;
  struct MemberInfo
  {
    std::string original;
    std::string normalized;
    u64 size;
  };
  std::vector<MemberInfo> member_info;

  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    unzClose(zf);
    if (error)
      *error = "The downloaded ZIP archive is empty.";
    return false;
  }

  for (;;)
  {
    unz_file_info64 info = {};
    char name_buffer[4096] = {};
    if (unzGetCurrentFileInfo64(zf, &info, name_buffer, sizeof(name_buffer), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
      unzClose(zf);
      if (error)
        *error = "Failed to read ZIP member information.";
      return false;
    }

    const std::string original(name_buffer);
    const std::optional<std::string> normalized = NormalizeArchiveMember(original);
    if (!normalized.has_value())
    {
      unzClose(zf);
      if (error)
        *error = "The package contains an unsafe archive path.";
      return false;
    }

    const bool is_directory = (!original.empty() && (original.back() == '/' || original.back() == '\\'));
    if (!is_directory)
    {
      if (info.uncompressed_size > MAX_PACKAGE_FILE_SIZE)
      {
        unzClose(zf);
        if (error)
          *error = "The package contains an unexpectedly large file.";
        return false;
      }

      members.push_back(normalized.value());
      member_info.push_back({original, normalized.value(), static_cast<u64>(info.uncompressed_size)});
    }

    const int next = unzGoToNextFile(zf);
    if (next == UNZ_END_OF_LIST_OF_FILE)
      break;
    if (next != UNZ_OK)
    {
      unzClose(zf);
      if (error)
        *error = "Failed while scanning the ZIP archive.";
      return false;
    }
  }

  std::optional<std::string> shader_root = FindNamedDirectoryRoot(members, "Shaders");
  if (!shader_root.has_value())
    shader_root = FindFallbackRoot(members, false);

  std::optional<std::string> texture_root = FindNamedDirectoryRoot(members, "Textures");
  if (!texture_root.has_value())
    texture_root = FindFallbackRoot(members, true);

  if (!shader_root.has_value())
  {
    unzClose(zf);
    if (error)
      *error = "No ReShade FX shader directory could be found in the package.";
    return false;
  }

  std::string shader_suffix = GetPathAfterDirectory(package.install_path, "Shaders");
  if (!package.install_path.empty() && shader_suffix.empty() &&
      NormalizeSlashPath(package.install_path).find("Shaders") == std::string::npos)
  {
    shader_suffix = SanitizePackageName(package.name);
  }

  std::string texture_suffix = GetPathAfterDirectory(package.texture_install_path, "Textures");
  if (!package.texture_install_path.empty() && texture_suffix.empty() &&
      NormalizeSlashPath(package.texture_install_path).find("Textures") == std::string::npos)
  {
    texture_suffix = SanitizePackageName(package.name);
  }

  std::vector<ArchiveInstallFile> install_files;
  u64 total_size = 0;

  for (const MemberInfo& member : member_info)
  {
    std::optional<std::string> relative;
    std::string destination;

    if ((relative = GetRelativeToRoot(member.normalized, shader_root.value())).has_value())
    {
      const std::string basename = GetBaseName(relative.value());
      if (EndsWithNoCase(basename, ".fx"))
      {
        if (ContainsNoCase(package.deny_effect_files, basename))
          continue;

        if (!package.effect_files.empty() && ContainsNoCase(package.effect_files, basename) &&
            !ContainsNoCase(selected_effects, basename))
        {
          continue;
        }
      }

      destination = JoinRelativePath("Shaders", JoinRelativePath(shader_suffix, relative.value()));
    }
    else if (texture_root.has_value() &&
             (relative = GetRelativeToRoot(member.normalized, texture_root.value())).has_value())
    {
      destination = JoinRelativePath("Textures", JoinRelativePath(texture_suffix, relative.value()));
    }
    else
    {
      continue;
    }

    if (!IsSafeRelativePath(destination))
    {
      unzClose(zf);
      if (error)
        *error = "The package produced an unsafe installation path.";
      return false;
    }

    total_size += member.size;
    if (total_size > MAX_PACKAGE_EXTRACTED_SIZE)
    {
      unzClose(zf);
      if (error)
        *error = "The package expands beyond ArcadeDuck's safety limit.";
      return false;
    }

    install_files.push_back({member.original, NormalizeSlashPath(destination), member.size});
  }

  if (install_files.empty())
  {
    unzClose(zf);
    if (error)
      *error = "No installable files remained after package filtering.";
    return false;
  }

  const s32 installed_index = findInstalledPackage(package);
  const InstalledPackage* old_package =
    (installed_index >= 0) ? &m_installed_packages[static_cast<size_t>(installed_index)] : nullptr;
  const auto old_package_owns = [old_package](std::string_view relative_path) {
    return old_package && ContainsNoCase(old_package->files, relative_path);
  };

  const std::string reshade_root = Path::Combine(EmuFolders::Shaders, "reshade");
  const std::string packages_root = Path::Combine(reshade_root, ".packages");
  const std::string slug = SanitizePackageName(package.name);
  const std::string staging_root = Path::Combine(packages_root, ".staging-" + slug);

  FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
  if (!FileSystem::EnsureDirectoryExists(staging_root.c_str(), true))
  {
    unzClose(zf);
    if (error)
      *error = "Failed to create the package staging directory.";
    return false;
  }

  for (const ArchiveInstallFile& file : install_files)
  {
    const std::string final_path = Path::Combine(reshade_root, ToNativePath(file.destination_relative));
    if (FileSystem::FileExists(final_path.c_str()) && !old_package_owns(file.destination_relative))
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "Installation would overwrite a shader or texture not owned by this package: " + file.destination_relative;
      return false;
    }

    if (unzLocateFile(zf, file.archive_name.c_str(), 0) != UNZ_OK || unzOpenCurrentFile(zf) != UNZ_OK)
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "Failed to reopen a ZIP member during extraction.";
      return false;
    }

    std::vector<u8> contents(static_cast<size_t>(file.uncompressed_size));
    size_t read_offset = 0;
    while (read_offset < contents.size())
    {
      const unsigned request_size =
        static_cast<unsigned>(std::min<size_t>(contents.size() - read_offset, 64 * 1024));
      const int read = unzReadCurrentFile(zf, contents.data() + read_offset, request_size);
      if (read <= 0)
      {
        unzCloseCurrentFile(zf);
        unzClose(zf);
        FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
        if (error)
          *error = "Failed while extracting a ZIP member.";
        return false;
      }
      read_offset += static_cast<size_t>(read);
    }

    if (unzCloseCurrentFile(zf) != UNZ_OK)
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "ZIP CRC validation failed during extraction.";
      return false;
    }

    const std::string staging_path = Path::Combine(staging_root, ToNativePath(file.destination_relative));
    const std::string staging_dir(Path::GetDirectory(staging_path));
    if (!staging_dir.empty() && !FileSystem::EnsureDirectoryExists(staging_dir.c_str(), true))
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "Failed to create a staging subdirectory.";
      return false;
    }

    if (!FileSystem::WriteBinaryFile(staging_path.c_str(), contents.data(), contents.size()))
    {
      unzClose(zf);
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "Failed to write a staged package file.";
      return false;
    }
  }

  unzClose(zf);

  for (const ArchiveInstallFile& file : install_files)
  {
    const std::string staging_path = Path::Combine(staging_root, ToNativePath(file.destination_relative));
    const std::string final_path = Path::Combine(reshade_root, ToNativePath(file.destination_relative));
    const std::string final_dir(Path::GetDirectory(final_path));

    if (!final_dir.empty() && !FileSystem::EnsureDirectoryExists(final_dir.c_str(), true))
    {
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "Failed to create the final package directory.";
      return false;
    }

    if (!FileSystem::CopyFilePath(staging_path.c_str(), final_path.c_str(), true))
    {
      FileSystem::RecursiveDeleteDirectory(staging_root.c_str());
      if (error)
        *error = "Failed to copy a package file into the shader directory.";
      return false;
    }
  }

  if (old_package)
  {
    for (const std::string& old_file : old_package->files)
    {
      const bool still_present =
        std::any_of(install_files.begin(), install_files.end(),
                    [&old_file](const ArchiveInstallFile& current) {
                      return EqualNoCase(old_file, current.destination_relative);
                    });
      if (still_present || !IsSafeRelativePath(old_file))
        continue;

      const std::string old_path = Path::Combine(reshade_root, ToNativePath(old_file));
      if (FileSystem::FileExists(old_path.c_str()))
        FileSystem::DeleteFile(old_path.c_str());
    }
  }

  FileSystem::RecursiveDeleteDirectory(staging_root.c_str());

  std::string manifest;
  manifest.append("PackageName=").append(package.name).append("\n");
  manifest.append("DownloadUrl=").append(package.download_url).append("\n");
  manifest.append("RepositoryUrl=").append(package.repository_url).append("\n");
  for (const std::string& effect : selected_effects)
    manifest.append("SelectedEffect=").append(effect).append("\n");
  for (const ArchiveInstallFile& file : install_files)
    manifest.append("File=").append(file.destination_relative).append("\n");

  std::string manifest_path;
  if (old_package)
  {
    manifest_path = old_package->manifest_path;
  }
  else
  {
    const std::string filename = slug + (package.section.empty() ? "" : "-" + package.section) + ".manifest";
    manifest_path = Path::Combine(packages_root, filename);
  }

  if (!FileSystem::WriteStringToFile(manifest_path.c_str(), manifest))
  {
    if (error)
      *error = "The package files were installed, but ArcadeDuck could not write the package manifest.";
    return false;
  }

  return true;
}

bool ReShadePackageManagerDialog::removeInstalledPackage(const InstalledPackage& package, std::string* error)
{
  const std::string reshade_root = Path::Combine(EmuFolders::Shaders, "reshade");

  for (const std::string& relative : package.files)
  {
    if (!IsSafeRelativePath(relative))
    {
      if (error)
        *error = "The package manifest contains an unsafe path and was not removed.";
      return false;
    }

    const std::string path = Path::Combine(reshade_root, ToNativePath(relative));
    if (FileSystem::FileExists(path.c_str()) && !FileSystem::DeleteFile(path.c_str()))
    {
      if (error)
        *error = "Failed to remove package file: " + relative;
      return false;
    }
  }

  if (FileSystem::FileExists(package.manifest_path.c_str()) && !FileSystem::DeleteFile(package.manifest_path.c_str()))
  {
    if (error)
      *error = "Failed to remove the package manifest.";
    return false;
  }

  return true;
}

void ReShadePackageManagerDialog::installOrUpdateSelectedPackage()
{
  const std::optional<size_t> selected = getSelectedPackageIndex();
  if (!selected.has_value())
    return;

  EffectPackage& package = m_packages[selected.value()];
  if (!package.from_catalog)
    return;

  if (!package.compatibility_scanned)
  {
    std::string scan_error;
    if (!scanPackageCompatibility(&package, &scan_error))
    {
      if (scan_error != "Download cancelled or failed.")
        QMessageBox::critical(this, tr("Compatibility Check Failed"), QString::fromStdString(scan_error));
      return;
    }

    updateSelectionDetails();
  }

  const std::vector<std::string> selected_effects = getCheckedEffects();
  if (selected_effects.empty())
  {
    QMessageBox::information(this, tr("Install Package"),
                             tr("No compatible effects are selected for installation."));
    return;
  }

  std::string error;
  if (!installPackage(package, selected_effects, &error))
  {
    if (error != "Download cancelled or failed.")
      QMessageBox::critical(this, tr("Package Installation Failed"), QString::fromStdString(error));
    return;
  }

  refreshInstalledPackages();
  rebuildPackageList(package.download_url);
  g_emu_thread->reloadPostProcessingShaders();

  QMessageBox::information(this, tr("ReShade Effect Packages"),
                           tr("%1 was installed successfully.").arg(QString::fromStdString(package.name)));
}

void ReShadePackageManagerDialog::removeSelectedPackage()
{
  const std::optional<size_t> selected = getSelectedPackageIndex();
  if (!selected.has_value())
    return;

  const EffectPackage package = m_packages[selected.value()];
  const s32 installed_index = findInstalledPackage(package);
  if (installed_index < 0)
    return;

  if (QMessageBox::question(
        this, tr("Remove Package"),
        tr("Remove %1 and the shader/texture files recorded in its ArcadeDuck package manifest?\n\n"
           "Shaders from this package that are currently in a post-processing chain may fail to reload.")
          .arg(QString::fromStdString(package.name)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
  {
    return;
  }

  const InstalledPackage installed = m_installed_packages[static_cast<size_t>(installed_index)];
  std::string error;
  if (!removeInstalledPackage(installed, &error))
  {
    QMessageBox::critical(this, tr("Package Removal Failed"), QString::fromStdString(error));
    return;
  }

  refreshInstalledPackages();
  rebuildPackageList(package.download_url);
  g_emu_thread->reloadPostProcessingShaders();

  QMessageBox::information(this, tr("ReShade Effect Packages"),
                           tr("%1 was removed.").arg(QString::fromStdString(package.name)));
}
