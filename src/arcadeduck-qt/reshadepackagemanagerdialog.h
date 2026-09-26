// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "common/types.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QtWidgets/QDialog>

class HTTPDownloader;
class QLabel;
class QListWidget;
class QPushButton;
class QTreeWidget;

class ReShadePackageManagerDialog final : public QDialog
{
public:
  explicit ReShadePackageManagerDialog(QWidget* parent = nullptr);
  ~ReShadePackageManagerDialog() override;

private:
  enum class EffectCompatibilityStatus
  {
    Unchecked,
    Compatible,
    Unsupported,
    Unknown
  };

  struct EffectCompatibility
  {
    std::string effect;
    EffectCompatibilityStatus status = EffectCompatibilityStatus::Unchecked;
    std::string reason;
  };

  struct EffectPackage
  {
    std::string section;
    std::string name;
    std::string description;
    std::string install_path;
    std::string texture_install_path;
    std::string download_url;
    std::string repository_url;
    std::vector<std::string> effect_files;
    std::vector<std::string> deny_effect_files;
    std::vector<EffectCompatibility> compatibility;
    bool compatibility_scanned = false;
    bool from_catalog = true;
  };

  struct InstalledPackage
  {
    std::string manifest_path;
    std::string package_name;
    std::string download_url;
    std::string repository_url;
    std::vector<std::string> files;
    std::vector<std::string> selected_effects;
  };

  struct ArchiveInstallFile
  {
    std::string archive_name;
    std::string destination_relative;
    u64 uncompressed_size = 0;
  };

  void refreshCatalog();
  void refreshInstalledPackages();
  void rebuildPackageList(std::string_view preserve_download_url = {});
  void updateSelectionDetails();

  void checkSelectedPackageCompatibility();
  void installOrUpdateSelectedPackage();
  void removeSelectedPackage();
  void openSelectedRepository();

  std::optional<size_t> getSelectedPackageIndex() const;
  s32 findInstalledPackage(const EffectPackage& package) const;

  bool downloadUrl(const std::string& url, const QString& status_text, std::vector<u8>* output_data);
  bool parseCatalog(std::string_view text, std::vector<EffectPackage>* packages, std::string* error) const;
  bool readInstalledManifest(const std::string& path, InstalledPackage* package) const;
  bool scanPackageCompatibility(EffectPackage* package, std::string* error);
  bool installPackage(const EffectPackage& package, const std::vector<std::string>& selected_effects, std::string* error);
  bool removeInstalledPackage(const InstalledPackage& package, std::string* error);

  const EffectCompatibility* findEffectCompatibility(const EffectPackage& package, std::string_view effect) const;
  std::vector<std::string> getCheckedEffects() const;

  std::unique_ptr<HTTPDownloader> m_http;
  std::vector<EffectPackage> m_packages;
  std::vector<InstalledPackage> m_installed_packages;

  QTreeWidget* m_package_list = nullptr;
  QLabel* m_description = nullptr;
  QLabel* m_status = nullptr;
  QListWidget* m_effect_list = nullptr;
  QPushButton* m_check_button = nullptr;
  QPushButton* m_repository_button = nullptr;
  QPushButton* m_install_button = nullptr;
  QPushButton* m_remove_button = nullptr;

  std::string m_cached_package_url;
  std::vector<u8> m_cached_package_archive;
};
