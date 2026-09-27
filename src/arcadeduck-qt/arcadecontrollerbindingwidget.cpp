// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only

#include "arcadecontrollerbindingwidget.h"

#include "controllersettingswindow.h"
#include "inputbindingwidgets.h"
#include "qtutils.h"

#include "core/arcade/arcade_control_registry.h"
#include "core/host.h"
#include "core/settings.h"

#include "common/path.h"

#include <QtCore/QCollator>
#include <QtCore/QDir>
#include <QtCore/QSignalBlocker>
#include <QtCore/QPointer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

namespace {

void ConfigureComboBoxPopup(QComboBox* combo, int max_visible_items = 18)
{
  if (!combo)
    return;

  combo->setMaxVisibleItems(max_visible_items);
  combo->view()->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

const QPixmap& GetCachedArtworkPixmap(const QString& resource_path)
{
  static QHash<QString, QPixmap> cache;
  const auto iterator = cache.constFind(resource_path);
  if (iterator != cache.cend())
    return iterator.value();

  return cache.insert(resource_path, QPixmap(resource_path)).value();
}

bool IsLegacyPointerDeviceSetting(const QString& identifier)
{
  return identifier == QStringLiteral("Mouse") || identifier == QStringLiteral("SystemMouse") ||
         identifier == QStringLiteral("RawMouse") || identifier == QStringLiteral("Pointer-0") ||
         identifier == QStringLiteral("Pointer-1") || identifier == QStringLiteral("Pointer-2") ||
         identifier == QStringLiteral("Pointer-3");
}

QString StripDisplayDuplicateSuffix(QString name)
{
  const int suffix_start = name.lastIndexOf(QStringLiteral(" ("));
  if (suffix_start < 0 || !name.endsWith(QLatin1Char(')')))
    return name;

  bool is_number = false;
  name.mid(suffix_start + 2, name.size() - suffix_start - 3).toUInt(&is_number);
  return is_number ? name.left(suffix_start) : name;
}

QString ImportCrosshairImage(QWidget* parent, const QString& source_filename)
{
  const QFileInfo source_info(source_filename);
  if (!source_info.exists() || !source_info.isFile())
  {
    QMessageBox::critical(parent, QObject::tr("Crosshair Image Error"),
                          QObject::tr("The selected crosshair image does not exist."));
    return {};
  }

  const QString suffix = source_info.suffix().toLower();
  if (suffix != QStringLiteral("png"))
  {
    QMessageBox::critical(parent, QObject::tr("Crosshair Image Error"),
                          QObject::tr("The selected file is not a supported crosshair image."));
    return {};
  }

  const QString crosshair_directory = QString::fromStdString(EmuFolders::Crosshairs);
  if (!QDir().mkpath(crosshair_directory))
  {
    QMessageBox::critical(
      parent, QObject::tr("Crosshair Image Error"),
      QObject::tr("Failed to create the crosshair directory '%1'.").arg(QDir::toNativeSeparators(crosshair_directory)));
    return {};
  }

  QString stem = QString::fromStdString(Path::SanitizeFileName(source_info.completeBaseName().toStdString()));
  if (stem.isEmpty())
    stem = QStringLiteral("crosshair");

  QString managed_filename = stem + QLatin1Char('.') + suffix;
  QString destination = QDir(crosshair_directory).filePath(managed_filename);
  if (QFileInfo(source_filename) == QFileInfo(destination))
    return managed_filename;

  for (int duplicate_index = 2; QFile::exists(destination); duplicate_index++)
  {
    managed_filename = QStringLiteral("%1-%2.%3").arg(stem).arg(duplicate_index).arg(suffix);
    destination = QDir(crosshair_directory).filePath(managed_filename);
  }

  if (!QFile::copy(source_filename, destination))
  {
    QMessageBox::critical(
      parent, QObject::tr("Crosshair Image Error"),
      QObject::tr("Failed to copy crosshair image to '%1'.").arg(QDir::toNativeSeparators(destination)));
    return {};
  }

  return managed_filename;
}

QString ExtractVidPid(const QString& value)
{
  const int vid = value.indexOf(QStringLiteral("VID_"), 0, Qt::CaseInsensitive);
  const int pid = value.indexOf(QStringLiteral("PID_"), 0, Qt::CaseInsensitive);
  if (vid >= 0 && pid >= 0 && (vid + 8) <= value.size() && (pid + 8) <= value.size())
    return value.mid(vid + 4, 4).toUpper() + QLatin1Char(':') + value.mid(pid + 4, 4).toUpper();

  const int open = value.lastIndexOf(QLatin1Char('['));
  const int close = value.indexOf(QLatin1Char(']'), open);
  if (open >= 0 && close > open)
    return value.mid(open + 1, close - open - 1).toUpper();
  return {};
}

QString ExtractSerial(const QString& value)
{
  const int serial = value.indexOf(QStringLiteral(" - SN "));
  if (serial < 0)
    return {};
  const int end = value.indexOf(QLatin1Char('['), serial);
  return value.mid(serial + 6, end >= 0 ? end - serial - 6 : -1).trimmed();
}

bool IsGenericPointerName(const QString& name)
{
  return StripDisplayDuplicateSuffix(name).startsWith(QStringLiteral("Windows Raw Mouse"), Qt::CaseInsensitive);
}

class ScaledArtworkLabel final : public QLabel
{
public:
  explicit ScaledArtworkLabel(QWidget* parent) : QLabel(parent) {}

  void setSourcePixmap(const QPixmap& pixmap)
  {
    m_source_pixmap = pixmap;
    updateScaledPixmap();
  }

  QSize sizeHint() const override { return QSize(320, 230); }
  QSize minimumSizeHint() const override { return QSize(0, 0); }

protected:
  void resizeEvent(QResizeEvent* event) override
  {
    QLabel::resizeEvent(event);
    updateScaledPixmap();
  }

private:
  void updateScaledPixmap()
  {
    if (!m_source_pixmap.isNull() && !size().isEmpty())
      QLabel::setPixmap(m_source_pixmap.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }

  QPixmap m_source_pixmap;
};

constexpr const char* ARCADE_BINDING_KEYS[] = {
  "Up", "Down", "Left", "Right", "Button1", "Button2", "Button3", "Button4", "Button5", "Button6",
  "Coin", "Start", "TrackballX", "TrackballY", "GunX", "GunY", "Trigger", "Reload", "GSRIncrease",
  "GSRDecrease", "Steering", "Accelerator", "Brake", "Clutch", "GearUp", "GearDown", "Handbrake",
  "View", "Horn", "MusicNext", "MusicPrevious", "Select", "SelectUp", "SelectDown", "SelectLeft",
  "SelectRight", "Enter", "Sensor"};
constexpr const char* ARCADE_CONTROLLER_VALUE_KEYS[] = {
  "InputMode", "PhysicalDevice", "PhysicalDeviceName", "Screen", "XSensitivity", "YSensitivity", "InvertX",
  "InvertY", "OffscreenReload", "CrosshairEnabled", "CrosshairImagePath", "CrosshairSize", "CrosshairScale",
  "SindenBorder", "SindenBorderWidth", "SteeringDeadzone", "SteeringSensitivity", "AcceleratorDeadzone",
  "AcceleratorSensitivity", "BrakeDeadzone", "BrakeSensitivity", "ClutchDeadzone", "ClutchSensitivity"};

} // namespace

ArcadeControllerBindingWidget::ArcadeControllerBindingWidget(QWidget* parent, ControllerSettingsWindow* dialog, u32 port)
  : QWidget(parent), m_dialog(dialog), m_port_number(port)
{
  rebuildPage();
}

QString ArcadeControllerBindingWidget::getDisplayName() const
{
  const Arcade::ArcadeControllerTypeInfo* info = Arcade::GetArcadeControllerTypeInfo(getType());
  return info ? QString::fromUtf8(info->display_name.data(), static_cast<int>(info->display_name.size())) : tr("Not Connected");
}

std::string ArcadeControllerBindingWidget::getConfigSection() const
{
  return std::string("ArcadeControllerPort") + std::to_string(m_port_number + 1);
}

Arcade::ArcadeControllerType ArcadeControllerBindingWidget::getType() const
{
  const std::string section = getConfigSection();
  const std::string type_name = m_dialog->getStringValue(section.c_str(), "Type", "none");
  for (const Arcade::ArcadeControllerTypeInfo& info : Arcade::GetArcadeControllerTypeInfos())
  {
    if (info.name == type_name)
      return info.type;
  }
  return Arcade::ArcadeControllerType::None;
}

bool ArcadeControllerBindingWidget::usesNativeDevice() const
{
  const std::string section = getConfigSection();
  return m_dialog->getStringValue(section.c_str(), "InputMode", "BoundAxis") == "NativeRawInputDevice";
}

void ArcadeControllerBindingWidget::setStringValue(const char* key, const char* value)
{
  const std::string section = getConfigSection();
  if (m_dialog->getStringValue(section.c_str(), key, "") != value)
    m_dialog->setStringValue(section.c_str(), key, value);
}

void ArcadeControllerBindingWidget::setBoolValue(const char* key, bool value)
{
  const std::string section = getConfigSection();
  if (m_dialog->getBoolValue(section.c_str(), key, !value) != value)
    m_dialog->setBoolValue(section.c_str(), key, value);
}

void ArcadeControllerBindingWidget::setIntValue(const char* key, s32 value)
{
  const std::string section = getConfigSection();
  if (m_dialog->getIntValue(section.c_str(), key, value - 1) != value)
    m_dialog->setIntValue(section.c_str(), key, value);
}

void ArcadeControllerBindingWidget::clearAllBindingValues()
{
  const std::string section = getConfigSection();
  for (const char* key : ARCADE_BINDING_KEYS)
    m_dialog->clearSettingValue(section.c_str(), key);
}

void ArcadeControllerBindingWidget::clearControllerSpecificValues()
{
  const std::string section = getConfigSection();
  for (const char* key : ARCADE_CONTROLLER_VALUE_KEYS)
    m_dialog->clearSettingValue(section.c_str(), key);
}

void ArcadeControllerBindingWidget::rebuildLayoutList()
{
  const Arcade::ArcadeControllerType type = getType();
  const std::string section = getConfigSection();
  const std::string current_layout = m_dialog->getStringValue(section.c_str(), "Layout", "");
  QSignalBlocker blocker(m_layout_combo);
  m_layout_combo->clear();
  if (type == Arcade::ArcadeControllerType::None)
  {
    m_layout_combo->addItem(tr("Not Applicable"), QString());
    m_layout_combo->setCurrentIndex(0);
    m_layout_combo->setEnabled(false);
    return;
  }

  std::vector<const Arcade::ArcadeControlLayoutInfo*> matching_layouts;
  for (const Arcade::ArcadeControlLayoutInfo& info : Arcade::GetArcadeControlLayoutInfos())
  {
    if (info.controller_type == type)
      matching_layouts.push_back(&info);
  }

  QCollator collator;
  collator.setCaseSensitivity(Qt::CaseInsensitive);
  collator.setNumericMode(true);
  std::sort(matching_layouts.begin(), matching_layouts.end(), [&collator](const auto* lhs, const auto* rhs) {
    const QString lhs_name = QString::fromUtf8(lhs->display_name.data(), static_cast<int>(lhs->display_name.size()));
    const QString rhs_name = QString::fromUtf8(rhs->display_name.data(), static_cast<int>(rhs->display_name.size()));
    return collator.compare(lhs_name, rhs_name) < 0;
  });

  m_layout_combo->setEnabled(true);
  for (const Arcade::ArcadeControlLayoutInfo* info : matching_layouts)
  {
    const QString key = QString::fromUtf8(info->layout_key.data(), static_cast<int>(info->layout_key.size()));
    m_layout_combo->addItem(
      QString::fromUtf8(info->display_name.data(), static_cast<int>(info->display_name.size())), key);
  }

  const Arcade::ArcadeControlLayoutInfo* const resolved_layout = Arcade::GetArcadeControlLayoutInfo(current_layout);
  const QString resolved_key =
    resolved_layout ?
      QString::fromUtf8(resolved_layout->layout_key.data(), static_cast<int>(resolved_layout->layout_key.size())) :
      QString::fromStdString(current_layout);
  const int index = m_layout_combo->findData(resolved_key);
  m_layout_combo->setCurrentIndex(index >= 0 ? index : 0);
}

void ArcadeControllerBindingWidget::addBinding(QFormLayout* layout, const char* key, const QString& label,
                                               InputBindingInfo::Type type)
{
  const std::string section = getConfigSection();
  layout->addRow(label, new InputBindingWidget(this, m_dialog->getEditingSettingsInterface(), type, section, key));
}

void ArcadeControllerBindingWidget::addBindingsPage(QVBoxLayout* layout)
{
  const Arcade::ArcadeControllerType type = getType();
  if (type == Arcade::ArcadeControllerType::None)
  {
    layout->addWidget(new QLabel(tr("This arcade controller port is not connected."), this));
    layout->addStretch(1);
    return;
  }

  const std::string section = getConfigSection();
  const std::string layout_key = m_dialog->getStringValue(section.c_str(), "Layout", "");
  const Arcade::ArcadeControlLayoutInfo* layout_info = Arcade::GetArcadeControlLayoutInfo(layout_key);

  QGroupBox* group = new QGroupBox(tr("Bindings"), this);
  QFormLayout* form = new QFormLayout(group);
  bool has_coin_binding = false;
  bool has_start_binding = false;

  const auto add_layout_bindings = [&]() {
    if (!layout_info || layout_info->controller_type != type)
      return false;

    for (const Arcade::ArcadeControlLayoutBindingInfo& binding : layout_info->bindings)
    {
      has_coin_binding |= (binding.binding_key == "Coin");
      has_start_binding |= (binding.binding_key == "Start");

      const bool native_pointer_axis =
        usesNativeDevice() &&
        ((type == Arcade::ArcadeControllerType::Trackball &&
          (binding.binding_key == "TrackballX" || binding.binding_key == "TrackballY")) ||
         (type == Arcade::ArcadeControllerType::Lightgun &&
          (binding.binding_key == "GunX" || binding.binding_key == "GunY")));
      if (native_pointer_axis)
        continue;

      const InputBindingInfo::Type binding_type =
        binding.kind == Arcade::ArcadeControlBindingKind::Axis ? InputBindingInfo::Type::Axis :
                                                                 InputBindingInfo::Type::Button;
      addBinding(form, binding.binding_key.data(),
                 QString::fromUtf8(binding.display_name.data(), static_cast<int>(binding.display_name.size())),
                 binding_type);
    }
    return true;
  };

  const bool used_layout = add_layout_bindings();
  if (!used_layout)
  {
    if (type == Arcade::ArcadeControllerType::Tokimeki)
    {
      addBinding(form, "Up", tr("Up"), InputBindingInfo::Type::Button);
      addBinding(form, "Down", tr("Down"), InputBindingInfo::Type::Button);
      addBinding(form, "Left", tr("Left"), InputBindingInfo::Type::Button);
      addBinding(form, "Right", tr("Right"), InputBindingInfo::Type::Button);
      addBinding(form, "Button1", tr("Action / Select"), InputBindingInfo::Type::Button);
      addBinding(form, "GSRIncrease", tr("Increase Excitement"), InputBindingInfo::Type::Button);
      addBinding(form, "GSRDecrease", tr("Decrease Excitement"), InputBindingInfo::Type::Button);
    }
    else if (type == Arcade::ArcadeControllerType::Trackball)
    {
      if (!usesNativeDevice())
      {
        addBinding(form, "TrackballX", tr("Trackball X fallback"), InputBindingInfo::Type::Axis);
        addBinding(form, "TrackballY", tr("Trackball Y fallback"), InputBindingInfo::Type::Axis);
      }
      addBinding(form, "Button1", tr("Left Action / Select"), InputBindingInfo::Type::Button);
      addBinding(form, "Button2", tr("Right Action / Select"), InputBindingInfo::Type::Button);
    }
    else if (type == Arcade::ArcadeControllerType::Lightgun)
    {
      if (!usesNativeDevice())
      {
        addBinding(form, "GunX", tr("Gun X fallback"), InputBindingInfo::Type::Axis);
        addBinding(form, "GunY", tr("Gun Y fallback"), InputBindingInfo::Type::Axis);
      }
      addBinding(form, "Trigger", tr("Trigger"), InputBindingInfo::Type::Button);
      addBinding(form, "Reload", tr("Reload"), InputBindingInfo::Type::Button);
    }
    else
    {
      form->addRow(new QLabel(tr("No compatible control layout is selected."), group));
    }
  }

  if (!has_coin_binding)
    addBinding(form, "Coin", tr("Coin"), InputBindingInfo::Type::Button);
  if (!has_start_binding)
    addBinding(form, "Start", tr("Start"), InputBindingInfo::Type::Button);

  QHBoxLayout* content = new QHBoxLayout();
  content->addWidget(group, 3);
  content->addWidget(createArtworkWidget(), 2, Qt::AlignCenter);
  layout->addLayout(content);
  layout->addStretch(1);
}

QWidget* ArcadeControllerBindingWidget::createArtworkWidget()
{
  QFrame* frame = new QFrame(this);
  frame->setFrameShape(QFrame::StyledPanel);
  frame->setMinimumSize(220, 170);
  frame->setMaximumWidth(400);
  frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  QVBoxLayout* frame_layout = new QVBoxLayout(frame);
  frame_layout->setContentsMargins(10, 10, 10, 10);
  ScaledArtworkLabel* artwork = new ScaledArtworkLabel(frame);
  artwork->setAlignment(Qt::AlignCenter);
  artwork->setWordWrap(true);

  const Arcade::ArcadeControllerType type = getType();
  const std::string section = getConfigSection();
  const std::string layout_key = m_dialog->getStringValue(section.c_str(), "Layout", "");
  const Arcade::ArcadeControlLayoutInfo* const layout_info = Arcade::GetArcadeControlLayoutInfo(layout_key);

  const char* resource = nullptr;
  if (layout_info && layout_info->layout_key == "bam2")
    resource = ":/controllers/ArcadeDuck_Bust_A_Move_controls.png";
  else if (type == Arcade::ArcadeControllerType::Arcade)
    resource = ":/controllers/arcade_controller.png";
  else if (type == Arcade::ArcadeControllerType::Trackball)
    resource = ":/controllers/trackball.png";
  else if (type == Arcade::ArcadeControllerType::Lightgun)
    resource = ":/controllers/lightgun.png";
  else if (type == Arcade::ArcadeControllerType::Driving)
    resource = ":/controllers/racing.png";
  else if (type == Arcade::ArcadeControllerType::Tokimeki)
    resource = ":/controllers/tokimeki.png";
  else if (type == Arcade::ArcadeControllerType::Mahjong)
    resource = ":/controllers/mahjong.png";

  const QString resource_path = QString::fromUtf8(resource ? resource : "");
  const bool resource_exists = QFile::exists(resource_path);
  const QPixmap& pixmap = GetCachedArtworkPixmap(resource_path);
  if (!resource_exists || pixmap.isNull())
  {
    artwork->setText(tr("Failed to load controller artwork: %1\nQFile::exists: %2")
                       .arg(resource_path, resource_exists ? tr("true") : tr("false")));
  }
  else
  {
    artwork->setSourcePixmap(pixmap);
  }

  frame_layout->addWidget(artwork, 1, Qt::AlignCenter);
  return frame;
}

void ArcadeControllerBindingWidget::addSettingsPage(QVBoxLayout* layout)
{
  const Arcade::ArcadeControllerType type = getType();
  if (type != Arcade::ArcadeControllerType::Trackball && type != Arcade::ArcadeControllerType::Lightgun &&
      type != Arcade::ArcadeControllerType::Driving)
  {
    layout->addWidget(new QLabel(tr("This controller type has no additional settings."), this));
    layout->addStretch(1);
    return;
  }
  const std::string section = getConfigSection();
  QGroupBox* group = new QGroupBox(tr("Controller Settings"), this);
  QFormLayout* form = new QFormLayout(group);

  if (type == Arcade::ArcadeControllerType::Driving)
  {
    const std::string layout_key = m_dialog->getStringValue(section.c_str(), "Layout", "");
    const Arcade::ArcadeControlLayoutInfo* layout_info = Arcade::GetArcadeControlLayoutInfo(layout_key);
    const auto has_axis = [layout_info](const char* key) {
      if (!layout_info)
        return true;

      return std::any_of(layout_info->bindings.begin(), layout_info->bindings.end(),
                         [key](const Arcade::ArcadeControlLayoutBindingInfo& binding) {
                           return binding.kind == Arcade::ArcadeControlBindingKind::Axis && binding.binding_key == key;
                         });
    };
    const auto add_axis_adjustments = [this, group, form, &section](const QString& name, const char* deadzone_key,
                                                                    const char* sensitivity_key) {
      QSpinBox* deadzone = new QSpinBox(group);
      deadzone->setRange(0, 50);
      deadzone->setSuffix("%");
      deadzone->setValue(m_dialog->getIntValue(section.c_str(), deadzone_key, 0));
      deadzone->setToolTip(
        tr("Ignores small movement at the neutral end of this axis, then rescales the remaining travel."));
      connect(deadzone, QOverload<int>::of(&QSpinBox::valueChanged), this,
              [this, deadzone_key](int value) { setIntValue(deadzone_key, value); });
      form->addRow(tr("%1 Deadzone").arg(name), deadzone);

      QSpinBox* sensitivity = new QSpinBox(group);
      sensitivity->setRange(1, 500);
      sensitivity->setSuffix("%");
      sensitivity->setValue(m_dialog->getIntValue(section.c_str(), sensitivity_key, 100));
      sensitivity->setToolTip(tr(
        "Adjusts the response curve while preserving full axis travel. 100% is linear and matches the default input."));
      connect(sensitivity, QOverload<int>::of(&QSpinBox::valueChanged), this,
              [this, sensitivity_key](int value) { setIntValue(sensitivity_key, value); });
      form->addRow(tr("%1 Sensitivity").arg(name), sensitivity);
    };

    if (has_axis("Steering"))
      add_axis_adjustments(tr("Steering"), "SteeringDeadzone", "SteeringSensitivity");
    if (has_axis("Accelerator"))
      add_axis_adjustments(tr("Accelerator"), "AcceleratorDeadzone", "AcceleratorSensitivity");
    if (has_axis("Brake"))
      add_axis_adjustments(tr("Brake"), "BrakeDeadzone", "BrakeSensitivity");
    if (has_axis("Clutch"))
      add_axis_adjustments(tr("Clutch"), "ClutchDeadzone", "ClutchSensitivity");

    layout->addWidget(group);
    layout->addStretch(1);
    return;
  }
    QComboBox* mode = new QComboBox(group);
    ConfigureComboBoxPopup(mode);
    mode->addItem(tr("Native Raw Input Device"), "NativeRawInputDevice"); mode->addItem(tr("Bound Axis"), "BoundAxis");
    mode->addItem(tr("Analog Cursor"), "AnalogCursor"); mode->addItem(tr("Analog Velocity"), "AnalogVelocity");
    mode->setCurrentIndex(mode->findData(QString::fromStdString(m_dialog->getStringValue(section.c_str(), "InputMode", "BoundAxis"))));
    connect(mode, &QComboBox::currentIndexChanged, this, [this, mode](int) {
      setStringValue("InputMode", mode->currentData().toString().toUtf8().constData());
      queueRebuild();
    });
    form->addRow(tr("Input Mode"), mode);
    QComboBox* device = new QComboBox(group);
    ConfigureComboBoxPopup(device);
    device->addItem(tr("No device"), "");
    for (const PointerDeviceInfo& info : m_dialog->getPointerDeviceList())
    {
      if (info.identifier.starts_with("RawInput:"))
        device->addItem(QString::fromStdString(info.display_name), QString::fromStdString(info.identifier));
    }
    QString selected_device = QString::fromStdString(m_dialog->getStringValue(section.c_str(), "PhysicalDevice", ""));
    if (IsLegacyPointerDeviceSetting(selected_device))
    {
      selected_device.clear();
      setStringValue("PhysicalDevice", "");
    }
    int selected_index = device->findData(selected_device);
    if (selected_index < 0 && selected_device.startsWith(QStringLiteral("RawInput:")))
    {
      const QString saved_name = QString::fromStdString(m_dialog->getStringValue(section.c_str(), "PhysicalDeviceName", ""));
      const QString saved_serial = ExtractSerial(saved_name);
      const QString saved_vid_pid = ExtractVidPid(selected_device).isEmpty() ? ExtractVidPid(saved_name) :
                                                                              ExtractVidPid(selected_device);
      const QString saved_display_name = StripDisplayDuplicateSuffix(saved_name);
      int matching_index = -1;
      for (int index = 1; index < device->count(); index++)
      {
        const QString connected_name = device->itemText(index);
        const QString connected_identifier = device->itemData(index).toString();
        const QString connected_serial = ExtractSerial(connected_name);
        const QString connected_vid_pid = ExtractVidPid(connected_identifier).isEmpty() ? ExtractVidPid(connected_name) :
                                                                                           ExtractVidPid(connected_identifier);
        const bool serial_match = !saved_serial.isEmpty() && saved_serial == connected_serial;
        const bool vid_pid_match = !saved_vid_pid.isEmpty() && saved_vid_pid == connected_vid_pid;
        const bool name_match = !saved_display_name.isEmpty() && !IsGenericPointerName(saved_display_name) &&
                                saved_display_name == StripDisplayDuplicateSuffix(connected_name);
        if (!serial_match && !vid_pid_match && !name_match)
          continue;
        if (matching_index >= 0)
        {
          matching_index = -2;
          break;
        }
        matching_index = index;
      }
      if (matching_index >= 0)
      {
        selected_index = matching_index;
        selected_device = device->itemData(selected_index).toString();
        setStringValue("PhysicalDevice", selected_device.toUtf8().constData());
        setStringValue("PhysicalDeviceName", device->itemText(selected_index).toUtf8().constData());
      }
    }
    if (selected_index >= 0 && !selected_device.isEmpty())
      setStringValue("PhysicalDeviceName", device->itemText(selected_index).toUtf8().constData());
    else if (selected_device.startsWith(QStringLiteral("RawInput:")))
    {
      const QString last_known_name = QString::fromStdString(m_dialog->getStringValue(section.c_str(), "PhysicalDeviceName", ""));
      device->addItem(last_known_name.isEmpty() ? tr("Assigned Raw Input device - Disconnected") :
                                                tr("%1 - Disconnected").arg(last_known_name), selected_device);
    }
    device->setCurrentIndex(device->findData(selected_device));
    connect(device, &QComboBox::currentIndexChanged, this, [this, device](int) {
      const QString identifier = device->currentData().toString();
      setStringValue("PhysicalDevice", identifier.toUtf8().constData());
      if (identifier.startsWith(QStringLiteral("RawInput:")))
        setStringValue("PhysicalDeviceName", device->currentText().toUtf8().constData());
    });
    QWidget* device_row = new QWidget(group);
    QHBoxLayout* device_layout = new QHBoxLayout(device_row);
    device_layout->setContentsMargins(0, 0, 0, 0);
    device_layout->addWidget(device, 1);
    form->addRow(tr("Physical device"), device_row);
    for (const auto& [label, key] : {std::pair<QString, const char*>{tr("X sensitivity"), "XSensitivity"}, {tr("Y sensitivity"), "YSensitivity"}})
    {
      QSpinBox* spin = new QSpinBox(group); spin->setRange(1, 500); spin->setSuffix("%"); spin->setValue(m_dialog->getIntValue(section.c_str(), key, 100));
      connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, key](int v) { setIntValue(key, v); }); form->addRow(label, spin);
    }
    for (const auto& [label, key] : {std::pair<QString, const char*>{tr("Invert X"), "InvertX"}, {tr("Invert Y"), "InvertY"}})
    {
      QCheckBox* check = new QCheckBox(group); check->setChecked(m_dialog->getBoolValue(section.c_str(), key, false));
      connect(check, &QCheckBox::toggled, this, [this, key](bool v) { setBoolValue(key, v); }); form->addRow(label, check);
    }
    form->addRow(tr("Routing"), new QLabel(tr("This port: %1").arg(m_port_number + 1), group));
    if (type == Arcade::ArcadeControllerType::Lightgun)
    {
      QCheckBox* reload = new QCheckBox(group); reload->setChecked(m_dialog->getBoolValue(section.c_str(), "OffscreenReload", true));
      connect(reload, &QCheckBox::toggled, this, [this](bool v) { setBoolValue("OffscreenReload", v); }); form->addRow(tr("Off-screen reload"), reload);
      if (m_port_number == 0)
      {
        QCheckBox* sinden_border = new QCheckBox(group);
        sinden_border->setChecked(m_dialog->getBoolValue(section.c_str(), "SindenBorder", false));
        QSpinBox* sinden_border_width = new QSpinBox(group);
        sinden_border_width->setRange(1, 64);
        sinden_border_width->setSuffix(tr(" px"));
        sinden_border_width->setValue(m_dialog->getIntValue(section.c_str(), "SindenBorderWidth", 4));
        sinden_border_width->setEnabled(sinden_border->isChecked());
        connect(sinden_border, &QCheckBox::toggled, this, [this, sinden_border_width](bool enabled) {
          setBoolValue("SindenBorder", enabled);
          sinden_border_width->setEnabled(enabled);
        });
        connect(sinden_border_width, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this](int value) { setIntValue("SindenBorderWidth", value); });
        form->addRow(tr("Draw Sinden Border"), sinden_border);
        form->addRow(tr("Sinden Border Width"), sinden_border_width);
      }
      QCheckBox* crosshair = new QCheckBox(group); crosshair->setChecked(m_dialog->getBoolValue(section.c_str(), "CrosshairEnabled", false));
      connect(crosshair, &QCheckBox::toggled, this, [this](bool v) { setBoolValue("CrosshairEnabled", v); }); form->addRow(tr("Show crosshair"), crosshair);
      QLineEdit* image_path = new QLineEdit(QString::fromStdString(m_dialog->getStringValue(section.c_str(), "CrosshairImagePath", "")), group); image_path->setReadOnly(true);
      QPushButton* browse = new QPushButton(tr("Browse..."), group); QPushButton* builtin = new QPushButton(tr("Built-in"), group);
      QWidget* image_row = new QWidget(group); QHBoxLayout* image_layout = new QHBoxLayout(image_row); image_layout->setContentsMargins(0,0,0,0); image_layout->addWidget(image_path, 1); image_layout->addWidget(browse); image_layout->addWidget(builtin); form->addRow(tr("Crosshair image"), image_row);
      QSlider* crosshair_scale = new QSlider(Qt::Horizontal, group);
      crosshair_scale->setRange(0, 20);
      crosshair_scale->setSingleStep(1);
      crosshair_scale->setPageStep(1);
      const int saved_crosshair_scale =
        std::clamp(m_dialog->getIntValue(section.c_str(), "CrosshairScale", 100), 0, 200);
      crosshair_scale->setValue((saved_crosshair_scale + 5) / 10);
      QLabel* crosshair_scale_value = new QLabel(tr("%1%").arg(crosshair_scale->value() * 10), group);
      crosshair_scale_value->setMinimumWidth(
        crosshair_scale_value->fontMetrics().horizontalAdvance(QStringLiteral("200%")));
      crosshair_scale_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      QWidget* crosshair_scale_row = new QWidget(group);
      QHBoxLayout* crosshair_scale_layout = new QHBoxLayout(crosshair_scale_row);
      crosshair_scale_layout->setContentsMargins(0, 0, 0, 0);
      crosshair_scale_layout->addWidget(crosshair_scale, 1);
      crosshair_scale_layout->addWidget(crosshair_scale_value);
      form->addRow(tr("Crosshair scale"), crosshair_scale_row);
      connect(crosshair_scale, &QSlider::valueChanged, this, [this, crosshair_scale_value](int value) {
        const int percent = value * 10;
        crosshair_scale_value->setText(tr("%1%").arg(percent));
        setIntValue("CrosshairScale", percent);
      });
      connect(browse, &QPushButton::clicked, this, [this, image_path]() {
        QString initial_path = image_path->text();
        if (initial_path.isEmpty())
          initial_path = QString::fromStdString(EmuFolders::Crosshairs);
        else if (!QFileInfo(initial_path).isAbsolute())
          initial_path = QDir(QString::fromStdString(EmuFolders::Crosshairs)).filePath(initial_path);

        const QString path = QFileDialog::getOpenFileName(this, tr("Select Crosshair Image"), initial_path,
                                                          tr("PNG Images (*.png)"));
        if (path.isEmpty())
          return;

        const QString managed_filename = ImportCrosshairImage(this, path);
        if (managed_filename.isEmpty())
          return;

        image_path->setText(QDir::toNativeSeparators(managed_filename));
        const QByteArray utf8_path = managed_filename.toUtf8();
        setStringValue("CrosshairImagePath", utf8_path.constData());
      });
      connect(builtin, &QPushButton::clicked, this, [this, image_path]() { image_path->clear(); setStringValue("CrosshairImagePath", ""); });
      const auto set_crosshair_controls_enabled =
        [image_path, browse, builtin, crosshair_scale, crosshair_scale_value](bool enabled) {
          image_path->setEnabled(enabled);
          browse->setEnabled(enabled);
          builtin->setEnabled(enabled);
          const bool custom_image_enabled = enabled && !image_path->text().isEmpty();
          crosshair_scale->setEnabled(custom_image_enabled);
          crosshair_scale_value->setEnabled(custom_image_enabled);
        };
      set_crosshair_controls_enabled(crosshair->isChecked());
      connect(crosshair, &QCheckBox::toggled, this, set_crosshair_controls_enabled);
      connect(image_path, &QLineEdit::textChanged, this, [crosshair, set_crosshair_controls_enabled](const QString&) {
        set_crosshair_controls_enabled(crosshair->isChecked());
      });
      form->addRow(tr("Screen / routing"), new QLabel(tr("Display 1 (placeholder)"), group));
    }
  layout->addWidget(group); layout->addStretch(1);
}

void ArcadeControllerBindingWidget::clearBindings()
{
  m_dialog->beginSettingsBatch();
  clearAllBindingValues();
  m_dialog->endSettingsBatch();
  queueRebuild();
}

void ArcadeControllerBindingWidget::rebuildPage()
{
  if (QLayout* old_layout = layout())
  {
    while (QLayoutItem* item = old_layout->takeAt(0))
    {
      if (QWidget* widget = item->widget())
      {
        widget->setParent(nullptr);
        widget->deleteLater();
      }
      delete item;
    }
    delete old_layout;
  }
  QVBoxLayout* main = new QVBoxLayout(this);
  QGroupBox* header = new QGroupBox(tr("Arcade Controller Port %1").arg(m_port_number + 1), this);
  QHBoxLayout* header_layout = new QHBoxLayout(header);
  m_type_combo = new QComboBox(header); m_layout_combo = new QComboBox(header);
  ConfigureComboBoxPopup(m_type_combo, 10);
  ConfigureComboBoxPopup(m_layout_combo);
  for (const Arcade::ArcadeControllerTypeInfo& info : Arcade::GetArcadeControllerTypeInfos())
    m_type_combo->addItem(QString::fromUtf8(info.display_name.data(), static_cast<int>(info.display_name.size())), static_cast<int>(info.type));
  m_type_combo->setCurrentIndex(m_type_combo->findData(static_cast<int>(getType())));
  header_layout->addWidget(new QLabel(tr("Controller Type"), header)); header_layout->addWidget(m_type_combo);
  header_layout->addWidget(new QLabel(tr("Game / Layout"), header)); header_layout->addWidget(m_layout_combo);
  QPushButton* clear = new QPushButton(tr("Clear Mapping"), header);
  header_layout->addWidget(clear);
  main->addWidget(header);
  rebuildLayoutList();
  connect(m_type_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
    const auto* info = Arcade::GetArcadeControllerTypeInfo(
      static_cast<Arcade::ArcadeControllerType>(m_type_combo->currentData().toInt()));
    if (!info)
      return;

    const Arcade::ArcadeControlLayoutInfo* default_layout = nullptr;
    for (const Arcade::ArcadeControlLayoutInfo& candidate : Arcade::GetArcadeControlLayoutInfos())
    {
      if (candidate.controller_type == info->type)
      {
        default_layout = &candidate;
        break;
      }
    }

    m_dialog->beginSettingsBatch();
    clearAllBindingValues();
    clearControllerSpecificValues();
    setStringValue("Type", info->name.data());
    setStringValue("Layout", default_layout ? default_layout->layout_key.data() : "");
    m_dialog->endSettingsBatch();
    queueRebuild();
  });
  connect(m_layout_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
    const QString layout = m_layout_combo->currentData().toString();
    const QByteArray layout_utf8 = layout.toUtf8();
    const std::string section = getConfigSection();
    const Arcade::ArcadeControlLayoutInfo* const current_info =
      Arcade::GetArcadeControlLayoutInfo(m_dialog->getStringValue(section.c_str(), "Layout", ""));
    if (current_info && current_info->layout_key == layout_utf8.constData())
      return;

    m_dialog->beginSettingsBatch();
    clearAllBindingValues();
    setStringValue("Layout", layout_utf8.constData());
    m_dialog->endSettingsBatch();
    queueRebuild();
  });
  connect(clear, &QPushButton::clicked, this, &ArcadeControllerBindingWidget::clearBindings);
  m_tabs = new QTabWidget(this); QWidget* bindings = new QWidget(m_tabs); QWidget* settings = new QWidget(m_tabs);
  QVBoxLayout* bindings_layout = new QVBoxLayout(bindings); QVBoxLayout* settings_layout = new QVBoxLayout(settings);
  addBindingsPage(bindings_layout); addSettingsPage(settings_layout);
  m_tabs->addTab(bindings, tr("Bindings")); m_tabs->addTab(settings, tr("Settings")); main->addWidget(m_tabs);
}

void ArcadeControllerBindingWidget::queueRebuild()
{
  if (m_rebuild_queued)
    return;

  m_rebuild_queued = true;
  const QPointer<ArcadeControllerBindingWidget> guard(this);
  const QPointer<ControllerSettingsWindow> dialog_guard(m_dialog);
  QMetaObject::invokeMethod(
    this,
    [guard, dialog_guard]() {
      if (!guard)
        return;

      guard->m_rebuild_queued = false;
      guard->rebuildPage();
      if (dialog_guard)
        dialog_guard->updateArcadeListDescription(guard->getPortNumber(), guard->getDisplayName());
    },
    Qt::QueuedConnection);
}
