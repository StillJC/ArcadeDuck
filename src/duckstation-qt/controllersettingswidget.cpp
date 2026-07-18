#include "controllersettingswidget.h"
#include "collapsiblewidget.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#if defined(_WIN32)
#include "common/windows_headers.h"
#include <cfgmgr32.h>
#include <devpkey.h>
#include <setupapi.h>
#endif

static constexpr char INPUT_PROFILE_FILTER[] = "Input Profiles (*.ini)";

#if defined(_WIN32)

static constexpr DEVPROPKEY ARCADEDUCK_DEVPKEY_Device_DeviceDesc = {
  {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 2};

static constexpr DEVPROPKEY ARCADEDUCK_DEVPKEY_Device_FriendlyName = {
  {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

static bool IsGenericRawMouseDeviceName(const QString& name);

static QString GetSetupAPIDeviceProperty(HDEVINFO device_info_set, SP_DEVINFO_DATA* device_info_data, DWORD property)
{
  WCHAR property_buffer[512] = {};
  DWORD property_type = 0;

  if (!SetupDiGetDeviceRegistryPropertyW(device_info_set, device_info_data, property, &property_type,
                                         reinterpret_cast<PBYTE>(property_buffer), sizeof(property_buffer), nullptr))
  {
    return {};
  }

  if (property_buffer[0] == 0)
    return {};

  return QString::fromWCharArray(property_buffer);
}

static bool IsGenericRawMouseDeviceName(const QString& name)
{
  return name.isEmpty() || name == QStringLiteral("HID-compliant mouse") ||
         name == QStringLiteral("USB Input Device") || name == QStringLiteral("USB Composite Device") ||
         name == QStringLiteral("USB Root Hub (USB 3.0)") || name == QStringLiteral("Generic USB Hub") ||
         name.contains(QStringLiteral("Hub"), Qt::CaseInsensitive) ||
         name == QStringLiteral("HID-compliant vendor-defined device") ||
         name == QStringLiteral("HID-compliant consumer control device") ||
         name == QStringLiteral("HID-compliant system controller") || name == QStringLiteral("HID Keyboard Device");
}

static QString GetRawMouseVidPidToken(const QString& device_name)
{
  const qsizetype vid_pos = device_name.indexOf(QStringLiteral("VID_"), 0, Qt::CaseInsensitive);
  const qsizetype pid_pos = device_name.indexOf(QStringLiteral("PID_"), 0, Qt::CaseInsensitive);

  if (vid_pos < 0 || pid_pos < 0)
    return {};

  return QStringLiteral("%1&%2").arg(device_name.mid(vid_pos, 8).toUpper(), device_name.mid(pid_pos, 8).toUpper());
}

static QString GetRawMouseSiblingDeviceDisplayName(const QString& device_name)
{
  const QString vid_pid_token = GetRawMouseVidPidToken(device_name);
  if (vid_pid_token.isEmpty())
    return {};

  HDEVINFO device_info_set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
  if (device_info_set == INVALID_HANDLE_VALUE)
    return {};

  QString best_display_name;

  for (DWORD device_index = 0;; device_index++)
  {
    SP_DEVINFO_DATA device_info_data = {};
    device_info_data.cbSize = sizeof(device_info_data);

    if (!SetupDiEnumDeviceInfo(device_info_set, device_index, &device_info_data))
      break;

    WCHAR instance_id_buffer[512] = {};
    if (CM_Get_Device_IDW(device_info_data.DevInst, instance_id_buffer,
                          static_cast<ULONG>(std::size(instance_id_buffer)), 0) != CR_SUCCESS)
    {
      continue;
    }

    const QString instance_id = QString::fromWCharArray(instance_id_buffer).toUpper();
    if (!instance_id.contains(vid_pid_token))
      continue;

    QString display_name = GetSetupAPIDeviceProperty(device_info_set, &device_info_data, SPDRP_FRIENDLYNAME);

    if (display_name.isEmpty())
      display_name = GetSetupAPIDeviceProperty(device_info_set, &device_info_data, SPDRP_DEVICEDESC);

    if (!IsGenericRawMouseDeviceName(display_name))
    {
      best_display_name = display_name;
      break;
    }
  }

  SetupDiDestroyDeviceInfoList(device_info_set);
  return best_display_name;
}

static QString GetRawMouseSetupAPIDisplayName(const QString& device_name)
{
  const std::wstring interface_path = device_name.toStdWString();

  HDEVINFO device_info_set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
  if (device_info_set == INVALID_HANDLE_VALUE)
    return {};

  SP_DEVICE_INTERFACE_DATA interface_data = {};
  interface_data.cbSize = sizeof(interface_data);

  QString display_name;

  if (SetupDiOpenDeviceInterfaceW(device_info_set, interface_path.c_str(), 0, &interface_data))
  {
    DWORD required_size = 0;
    SetupDiGetDeviceInterfaceDetailW(device_info_set, &interface_data, nullptr, 0, &required_size, nullptr);

    if (required_size > 0)
    {
      std::vector<BYTE> detail_buffer(required_size);
      SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail_data =
        reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer.data());
      detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

      SP_DEVINFO_DATA device_info_data = {};
      device_info_data.cbSize = sizeof(device_info_data);

      if (SetupDiGetDeviceInterfaceDetailW(device_info_set, &interface_data, detail_data, required_size, nullptr,
                                           &device_info_data))
      {
        display_name = GetSetupAPIDeviceProperty(device_info_set, &device_info_data, SPDRP_FRIENDLYNAME);

        if (display_name.isEmpty())
          display_name = GetSetupAPIDeviceProperty(device_info_set, &device_info_data, SPDRP_DEVICEDESC);

        if (IsGenericRawMouseDeviceName(display_name))
        {
          if (const QString sibling_display_name = GetRawMouseSiblingDeviceDisplayName(device_name);
              !sibling_display_name.isEmpty())
          {
            display_name = sibling_display_name;
          }
        }
      }
    }
  }

  SetupDiDestroyDeviceInfoList(device_info_set);

  if (IsGenericRawMouseDeviceName(display_name))
    return {};

  return display_name;
}

static QString GetRawMouseDisplayName(const QString& device_name, u32 index)
{
  if (const QString setupapi_name = GetRawMouseSetupAPIDisplayName(device_name); !setupapi_name.isEmpty())
    return setupapi_name;

  DEVPROPTYPE property_type = 0;
  WCHAR property_buffer[512] = {};
  ULONG property_buffer_size = sizeof(property_buffer);

  const std::wstring interface_path = device_name.toStdWString();

  CONFIGRET result =
    CM_Get_Device_Interface_PropertyW(interface_path.c_str(), &ARCADEDUCK_DEVPKEY_Device_FriendlyName, &property_type,
                                      reinterpret_cast<PBYTE>(property_buffer), &property_buffer_size, 0);

  if (result != CR_SUCCESS || property_buffer[0] == 0)
  {
    property_buffer_size = sizeof(property_buffer);
    result =
      CM_Get_Device_Interface_PropertyW(interface_path.c_str(), &ARCADEDUCK_DEVPKEY_Device_DeviceDesc, &property_type,
                                        reinterpret_cast<PBYTE>(property_buffer), &property_buffer_size, 0);
  }

  if (result == CR_SUCCESS && property_buffer[0] != 0)
    return QString::fromWCharArray(property_buffer);

  const qsizetype vid_pos = device_name.indexOf(QStringLiteral("VID_"), 0, Qt::CaseInsensitive);
  const qsizetype pid_pos = device_name.indexOf(QStringLiteral("PID_"), 0, Qt::CaseInsensitive);

if (vid_pos >= 0 && pid_pos >= 0)
    return QStringLiteral("Wireless USB Mouse %1").arg(index + 1);

  return QStringLiteral("Raw Mouse %1").arg(index + 1);
}

static std::vector<std::pair<QString, QString>> GetRawMouseDeviceList()
{
  std::vector<std::pair<QString, QString>> devices;

  UINT device_count = 0;
  if (GetRawInputDeviceList(nullptr, &device_count, sizeof(RAWINPUTDEVICELIST)) != 0 || device_count == 0)
    return devices;

  std::vector<RAWINPUTDEVICELIST> raw_devices(device_count);
  if (GetRawInputDeviceList(raw_devices.data(), &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
    return devices;

  for (UINT i = 0; i < device_count; i++)
  {
    if (raw_devices[i].dwType != RIM_TYPEMOUSE)
      continue;

    UINT name_size = 0;
    GetRawInputDeviceInfoA(raw_devices[i].hDevice, RIDI_DEVICENAME, nullptr, &name_size);
    if (name_size == 0)
      continue;

    std::vector<char> name(name_size + 1);
    if (GetRawInputDeviceInfoA(raw_devices[i].hDevice, RIDI_DEVICENAME, name.data(), &name_size) ==
        static_cast<UINT>(-1))
      continue;

    const QString device_name = QString::fromLocal8Bit(name.data());
    const QString display_name = GetRawMouseDisplayName(device_name, static_cast<u32>(devices.size()));
    const QString setting_value = QStringLiteral("RawMouse:%1").arg(device_name);

    devices.emplace_back(display_name, setting_value);
  }

  return devices;
}
#endif

ControllerSettingsWidget::ControllerSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  createUi();

  connect(host_interface, &QtHostInterface::inputProfileLoaded, this, &ControllerSettingsWidget::onProfileLoaded);
}

ControllerSettingsWidget::~ControllerSettingsWidget() = default;

MultitapMode ControllerSettingsWidget::getMultitapMode()
{
  return Settings::ParseMultitapModeName(
           QtHostInterface::GetInstance()
             ->GetStringSettingValue("ControllerPorts", "MultitapMode",
                                     Settings::GetMultitapModeName(Settings::DEFAULT_MULTITAP_MODE))
             .c_str())
    .value_or(Settings::DEFAULT_MULTITAP_MODE);
}

QString ControllerSettingsWidget::getTabTitleForPort(u32 index, MultitapMode) const
{
  const ControllerType controller_type =
    Settings::ParseControllerTypeName(m_host_interface->GetStringSettingValue("Controller1", "Type").c_str())
      .value_or(ControllerType::AnalogController);

  u32 player_count = 2;

  switch (controller_type)
  {
    case ControllerType::NamcoGunCon:
      player_count = 3;
      break;

    case ControllerType::NeGcon:
    case ControllerType::SpecialSensor:
      player_count = 1;
      break;

    case ControllerType::AnalogController:
    case ControllerType::PlayStationMouse:
      player_count = 2;
      break;

    default:
      player_count = 1;
      break;
  }

  if (index >= player_count)
    return QString();

  return tr("Player %1").arg(index + 1);
}

void ControllerSettingsWidget::createUi()
{
  QGridLayout* layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  const MultitapMode multitap_mode = getMultitapMode();
  m_tab_widget = new QTabWidget(this);
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
    createPortSettingsUi(i, &m_port_ui[i], multitap_mode);

  layout->addWidget(m_tab_widget, 0, 0, 1, 1);

  setLayout(layout);
}

void ControllerSettingsWidget::updateMultitapControllerTitles()
{
  m_tab_widget->clear();

  const MultitapMode multitap_mode = getMultitapMode();
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
    createPortSettingsUi(i, &m_port_ui[i], multitap_mode);
}

void ControllerSettingsWidget::onProfileLoaded()
{
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
  {
    if (!m_port_ui[i].widget)
      continue;

    ControllerType ctype = Settings::ParseControllerTypeName(
                             m_host_interface
                               ->GetStringSettingValue(QStringLiteral("Controller%1").arg(i + 1).toStdString().c_str(),
                                                       QStringLiteral("Type").toStdString().c_str())
                               .c_str())
                             .value_or(ControllerType::None);

    {
      QSignalBlocker blocker(m_port_ui[i].controller_type);

      const int selected_index = m_port_ui[i].controller_type->findData(static_cast<int>(ctype));

      m_port_ui[i].controller_type->setCurrentIndex(selected_index >= 0 ? selected_index : 0);
    }
    createPortBindingSettingsUi(i, &m_port_ui[i], ctype);
  }
}

void ControllerSettingsWidget::reloadBindingButtons()
{
  for (PortSettingsUI& ui : m_port_ui)
  {
    InputBindingWidget* widget = ui.first_button;
    while (widget)
    {
      widget->reloadBinding();
      widget = widget->getNextWidget();
    }
  }
}

void ControllerSettingsWidget::createPortSettingsUi(int index, PortSettingsUI* ui, MultitapMode multitap_mode)
{
  if (ui->widget)
  {
    delete ui->widget;
    *ui = {};
  }

  const QString tab_title(getTabTitleForPort(index, multitap_mode));
  if (tab_title.isEmpty())
    return;

  ui->widget = new QWidget(m_tab_widget);
  ui->layout = new QVBoxLayout(ui->widget);

  QHBoxLayout* hbox = new QHBoxLayout();
  hbox->addWidget(new QLabel(tr("Controller Type:"), ui->widget));
  hbox->addSpacing(8);

  ui->controller_type = new QComboBox(ui->widget);

  const auto add_controller_type = [ui](ControllerType type) {
    ui->controller_type->addItem(qApp->translate("ControllerType", Settings::GetControllerTypeDisplayName(type)),
                                 static_cast<int>(type));
  };

  add_controller_type(ControllerType::AnalogController);

  if (index == 0)
    add_controller_type(ControllerType::SpecialSensor);

  add_controller_type(ControllerType::NamcoGunCon);
  add_controller_type(ControllerType::PlayStationMouse);
  add_controller_type(ControllerType::NeGcon);
  add_controller_type(ControllerType::DigitalController);
  add_controller_type(ControllerType::AnalogJoystick);
  add_controller_type(ControllerType::None);

  ControllerType ctype =
    Settings::ParseControllerTypeName(
      m_host_interface->GetStringSettingValue(TinyString::FromFormat("Controller%d", index + 1), "Type").c_str())
      .value_or(ControllerType::None);

  const int selected_index = ui->controller_type->findData(static_cast<int>(ctype));

  ui->controller_type->setCurrentIndex(selected_index >= 0 ? selected_index : 0);
  connect(ui->controller_type, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
          [this, index]() { onControllerTypeChanged(index); });

  hbox->addWidget(ui->controller_type, 1);
  ui->layout->addLayout(hbox);

  ui->bindings_scroll_area = new QScrollArea(ui->widget);
  ui->bindings_scroll_area->setWidgetResizable(true);
  ui->bindings_scroll_area->setFrameShape(QFrame::StyledPanel);
  ui->bindings_scroll_area->setFrameShadow(QFrame::Plain);

  createPortBindingSettingsUi(index, ui, ctype);

  ui->bindings_scroll_area->setWidget(ui->bindings_container);
  ui->layout->addWidget(ui->bindings_scroll_area, 1);

  hbox = new QHBoxLayout();
  QPushButton* load_profile_button = new QPushButton(tr("Load Profile"), ui->widget);
  connect(load_profile_button, &QPushButton::clicked, this, &ControllerSettingsWidget::onLoadProfileClicked);
  hbox->addWidget(load_profile_button);

  QPushButton* save_profile_button = new QPushButton(tr("Save Profile"), ui->widget);
  connect(save_profile_button, &QPushButton::clicked, this, &ControllerSettingsWidget::onSaveProfileClicked);
  hbox->addWidget(save_profile_button);

  hbox->addStretch(1);

  QPushButton* clear_all_button = new QPushButton(tr("Clear All"), ui->widget);
  clear_all_button->connect(clear_all_button, &QPushButton::clicked, [this, index]() {
    if (QMessageBox::question(this, tr("Clear Bindings"),
                              tr("Are you sure you want to clear all bound controls? This can not be reversed.")) !=
        QMessageBox::Yes)
    {
      return;
    }

    InputBindingWidget* widget = m_port_ui[index].first_button;
    while (widget)
    {
      widget->clearBinding();
      widget = widget->getNextWidget();
    }
  });

  QPushButton* rebind_all_button = new QPushButton(tr("Rebind All"), ui->widget);
  rebind_all_button->connect(rebind_all_button, &QPushButton::clicked, [this, index]() {
    if (QMessageBox::question(this, tr("Rebind All"),
                              tr("Are you sure you want to rebind all controls? All currently-bound controls will be "
                                 "irreversibly cleared. Rebinding will begin after confirmation.")) != QMessageBox::Yes)
    {
      return;
    }

    InputBindingWidget* widget = m_port_ui[index].first_button;
    while (widget)
    {
      widget->clearBinding();
      widget = widget->getNextWidget();
    }

    if (m_port_ui[index].first_button)
      m_port_ui[index].first_button->beginRebindAll();
  });

  hbox->addWidget(clear_all_button);
  hbox->addWidget(rebind_all_button);

  ui->layout->addLayout(hbox);

  ui->widget->setLayout(ui->layout);

  m_tab_widget->addTab(ui->widget, tab_title);
}

void ControllerSettingsWidget::createPortBindingSettingsUi(int index, PortSettingsUI* ui, ControllerType ctype)
{
  ui->bindings_container = new QWidget(ui->widget);

  QGridLayout* layout = new QGridLayout(ui->bindings_container);
  const auto buttons = Controller::GetButtonNames(ctype);
  const char* cname = Settings::GetControllerTypeName(ctype);

  InputBindingWidget* first_button = nullptr;
  InputBindingWidget* last_button = nullptr;

  int start_row = 0;
  if (!buttons.empty())
  {
    layout->addWidget(new QLabel(tr("Button Bindings:"), ui->bindings_container), start_row++, 0, 1, 4);

    const int num_rows = (static_cast<int>(buttons.size()) + 1) / 2;
    int current_row = 0;
    int current_column = 0;
    for (const auto& [button_name, button_code] : buttons)
    {
      if (current_row == num_rows)
      {
        current_row = 0;
        current_column += 2;
      }

      std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);
      std::string key_name = StringUtil::StdStringFromFormat("Button%s", button_name.c_str());
      QLabel* label = new QLabel(qApp->translate(cname, button_name.c_str()), ui->bindings_container);
      InputButtonBindingWidget* button = new InputButtonBindingWidget(m_host_interface, std::move(section_name),
                                                                      std::move(key_name), ui->bindings_container);
      layout->addWidget(label, start_row + current_row, current_column);
      layout->addWidget(button, start_row + current_row, current_column + 1);

      if (!first_button)
        first_button = button;
      if (last_button)
        last_button->setNextWidget(button);
      last_button = button;

      current_row++;
    }

    start_row += num_rows;
  }

  const auto axises = Controller::GetAxisNames(ctype);
  if (!axises.empty())
  {
    layout->addWidget(QtUtils::CreateHorizontalLine(ui->bindings_container), start_row++, 0, 1, 4);
    layout->addWidget(new QLabel(tr("Axis Bindings:"), ui->bindings_container), start_row++, 0, 1, 4);

    const int num_rows = (static_cast<int>(axises.size()) + 1) / 2;
    int current_row = 0;
    int current_column = 0;
    for (const auto& [axis_name, axis_code, axis_type] : axises)
    {
      if (current_row == num_rows)
      {
        current_row = 0;
        current_column += 2;
      }

      std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);
      std::string key_name = StringUtil::StdStringFromFormat("Axis%s", axis_name.c_str());
      QLabel* label = new QLabel(qApp->translate(cname, axis_name.c_str()), ui->bindings_container);
      InputAxisBindingWidget* button = new InputAxisBindingWidget(
        m_host_interface, std::move(section_name), std::move(key_name), axis_type, ui->bindings_container);
      layout->addWidget(label, start_row + current_row, current_column);
      layout->addWidget(button, start_row + current_row, current_column + 1);

      if (!first_button)
        first_button = button;
      if (last_button)
        last_button->setNextWidget(button);
      last_button = button;

      current_row++;
    }

    start_row += num_rows;
  }

  const u32 num_motors = Controller::GetVibrationMotorCount(ctype);
  if (num_motors > 0)
  {
    layout->addWidget(QtUtils::CreateHorizontalLine(ui->widget), start_row++, 0, 1, 4);

    std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);
    QLabel* label = new QLabel(tr("Rumble"), ui->bindings_container);
    InputRumbleBindingWidget* button =
      new InputRumbleBindingWidget(m_host_interface, std::move(section_name), "Rumble", ui->bindings_container);

    layout->addWidget(label, start_row, 0);
    layout->addWidget(button, start_row, 1);

    if (!first_button)
      first_button = button;
    if (last_button)
      last_button->setNextWidget(button);
    last_button = button;

    start_row++;
  }

  const Controller::SettingList settings = Controller::GetSettings(ctype);
  if (!settings.empty())
  {
    layout->addWidget(QtUtils::CreateHorizontalLine(ui->widget), start_row++, 0, 1, 4);

    for (const SettingInfo& si : settings)
    {
      std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);
      std::string key_name = si.key;
      const QString setting_tooltip = si.description ? qApp->translate(cname, si.description) : QString();

      switch (si.type)
      {
        case SettingInfo::Type::Boolean:
        {
          QCheckBox* cb = new QCheckBox(qApp->translate(cname, si.visible_name), ui->bindings_container);
          cb->setToolTip(setting_tooltip);
          SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, cb, std::move(section_name),
                                                       std::move(key_name), si.BooleanDefaultValue());
          layout->addWidget(cb, start_row, 0, 1, 4);
          start_row++;
        }
        break;

        case SettingInfo::Type::Integer:
        {
          QSpinBox* sb = new QSpinBox(ui->bindings_container);
          sb->setToolTip(setting_tooltip);
          sb->setMinimum(si.IntegerMinValue());
          sb->setMaximum(si.IntegerMaxValue());
          sb->setSingleStep(si.IntegerStepValue());
          SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, sb, std::move(section_name),
                                                      std::move(key_name), si.IntegerDefaultValue());
          layout->addWidget(new QLabel(qApp->translate(cname, si.visible_name), ui->bindings_container), start_row, 0);
          layout->addWidget(sb, start_row, 1, 1, 3);
          start_row++;
        }
        break;

        case SettingInfo::Type::Float:
        {
          QDoubleSpinBox* sb = new QDoubleSpinBox(ui->bindings_container);
          sb->setToolTip(setting_tooltip);
          sb->setMinimum(si.FloatMinValue());
          sb->setMaximum(si.FloatMaxValue());
          sb->setSingleStep(si.FloatStepValue());
          SettingWidgetBinder::BindWidgetToFloatSetting(m_host_interface, sb, std::move(section_name),
                                                        std::move(key_name), si.FloatDefaultValue());
          layout->addWidget(new QLabel(qApp->translate(cname, si.visible_name), ui->bindings_container), start_row, 0);
          layout->addWidget(sb, start_row, 1, 1, 3);
          start_row++;
        }
        break;

        case SettingInfo::Type::String:
        {
          QLineEdit* le = new QLineEdit(ui->bindings_container);
          le->setToolTip(setting_tooltip);
          SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, le, std::move(section_name),
                                                         std::move(key_name), si.StringDefaultValue());
          layout->addWidget(new QLabel(qApp->translate(cname, si.visible_name), ui->bindings_container), start_row, 0);
          layout->addWidget(le, start_row, 1, 1, 3);
          start_row++;
        }
        break;

        case SettingInfo::Type::Path:
        {
          QLineEdit* le = new QLineEdit(ui->bindings_container);
          le->setToolTip(setting_tooltip);
          QPushButton* browse_button = new QPushButton(tr("Browse..."), ui->bindings_container);
          SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, le, std::move(section_name),
                                                         std::move(key_name), si.StringDefaultValue());
          connect(browse_button, &QPushButton::clicked, [this, le]() {
            QString path = QFileDialog::getOpenFileName(this, tr("Select File"));
            if (!path.isEmpty())
              le->setText(path);
          });

          QHBoxLayout* hbox = new QHBoxLayout();
          hbox->addWidget(le, 1);
          hbox->addWidget(browse_button);

          layout->addWidget(new QLabel(qApp->translate(cname, si.visible_name), ui->bindings_container), start_row, 0);
          layout->addLayout(hbox, start_row, 1, 1, 3);
          start_row++;
        }
        break;
      }
    }
  }

  if (ctype == ControllerType::PlayStationMouse)
  {
    layout->addWidget(QtUtils::CreateHorizontalLine(ui->widget), start_row++, 0, 1, 4);
    layout->addWidget(new QLabel(tr("Trackball Settings:"), ui->bindings_container), start_row++, 0, 1, 4);

    const std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);

    QComboBox* trackball_device = new QComboBox(ui->bindings_container);
    trackball_device->addItem(tr("Disabled"), QStringLiteral("Disabled"));
    trackball_device->addItem(tr("System Mouse"), QStringLiteral("SystemMouse"));

#if defined(_WIN32)
    for (const auto& [display_name, setting_value] : GetRawMouseDeviceList())
      trackball_device->addItem(display_name, setting_value);
#endif

    SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, trackball_device, section_name, "TrackballDevice",
                                                   "Disabled");

    layout->addWidget(new QLabel(tr("Trackball Device"), ui->bindings_container), start_row, 0);
    layout->addWidget(trackball_device, start_row, 1, 1, 3);
    start_row++;
  }

  // Sinden border
  if (ctype == ControllerType::NamcoGunCon)
  {
    layout->addWidget(QtUtils::CreateHorizontalLine(ui->widget), start_row++, 0, 1, 4);
    layout->addWidget(new QLabel(tr("Lightgun Settings:"), ui->bindings_container), start_row++, 0, 1, 4);

    const std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);

    QComboBox* lightgun_device = new QComboBox(ui->bindings_container);
    lightgun_device->addItem(tr("Disabled"), QStringLiteral("Disabled"));
    lightgun_device->addItem(tr("System Mouse"), QStringLiteral("SystemMouse"));

#if defined(_WIN32)
    for (const auto& [display_name, setting_value] : GetRawMouseDeviceList())
      lightgun_device->addItem(display_name, setting_value);
#endif

    SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, lightgun_device, section_name, "LightgunDevice",
                                                   index == 0 ? "SystemMouse" : "Disabled");

    layout->addWidget(new QLabel(tr("Lightgun Device"), ui->bindings_container), start_row, 0);
    layout->addWidget(lightgun_device, start_row, 1, 1, 3);
    start_row++;

    if (index == 0)
    {
      QCheckBox* sinden_border = new QCheckBox(tr("Draw Sinden Border"), ui->bindings_container);
      SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, sinden_border, section_name, "SindenBorder",
                                                   false);

      layout->addWidget(sinden_border, start_row, 0, 1, 4);
      start_row++;

      QSpinBox* sinden_border_width = new QSpinBox(ui->bindings_container);
      sinden_border_width->setMinimum(1);
      sinden_border_width->setMaximum(64);
      sinden_border_width->setSuffix(tr(" px"));

      SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, sinden_border_width, section_name,
                                                  "SindenBorderWidth", 4);

      layout->addWidget(new QLabel(tr("Sinden Border Width"), ui->bindings_container), start_row, 0);
      layout->addWidget(sinden_border_width, start_row, 1, 1, 3);
      start_row++;
    }
  }

  // dummy row to fill remaining space
  layout->addWidget(new QWidget(ui->bindings_container), start_row, 0, 1, 4);
  layout->setRowStretch(start_row, 1);

  ui->bindings_scroll_area->setWidget(ui->bindings_container);
  ui->first_button = first_button;
}

void ControllerSettingsWidget::onControllerTypeChanged(int index)
{
  const QVariant type_data = m_port_ui[index].controller_type->currentData();

  if (!type_data.isValid())
    return;

  const int type_value = type_data.toInt();

  if (type_value < 0 || type_value >= static_cast<int>(ControllerType::Count))
  {
    return;
  }

  const ControllerType ctype = static_cast<ControllerType>(type_value);

  m_host_interface->SetStringSettingValue(TinyString::FromFormat("Controller%d", index + 1), "Type",
                                          Settings::GetControllerTypeName(ctype));

  m_host_interface->applySettings();

  if (index == 0)
  {
    QMetaObject::invokeMethod(this, [this]() { updateMultitapControllerTitles(); }, Qt::QueuedConnection);

    return;
  }

  createPortBindingSettingsUi(index, &m_port_ui[index], ctype);
}

void ControllerSettingsWidget::onLoadProfileClicked()
{
  const auto profile_names = m_host_interface->getInputProfileList();

  QMenu menu;

  QAction* browse = menu.addAction(tr("Browse..."));
  connect(browse, &QAction::triggered, [this]() {
    QString path =
      QFileDialog::getOpenFileName(this, tr("Select path to input profile ini"), QString(), tr(INPUT_PROFILE_FILTER));
    if (!path.isEmpty())
      m_host_interface->applyInputProfile(path);
  });

  if (!profile_names.empty())
    menu.addSeparator();

  for (const auto& [name, path] : profile_names)
  {
    QAction* action = menu.addAction(QString::fromStdString(name));
    QString path_qstr = QString::fromStdString(path);
    connect(action, &QAction::triggered, [this, path_qstr]() { m_host_interface->applyInputProfile(path_qstr); });
  }

  menu.exec(QCursor::pos());
}

void ControllerSettingsWidget::onSaveProfileClicked()
{
  const auto profile_names = m_host_interface->getInputProfileList();

  QMenu menu;

  QAction* new_action = menu.addAction(tr("New..."));
  connect(new_action, &QAction::triggered, [this]() {
    QString name = QInputDialog::getText(QtUtils::GetRootWidget(this), tr("Enter Input Profile Name"),
                                         tr("Enter Input Profile Name"));
    if (name.isEmpty())
    {
      QMessageBox::critical(QtUtils::GetRootWidget(this), tr("Error"),
                            tr("No name entered, input profile was not saved."));
      return;
    }

    m_host_interface->saveInputProfile(m_host_interface->getSavePathForInputProfile(name));
  });

  QAction* browse = menu.addAction(tr("Browse..."));
  connect(browse, &QAction::triggered, [this]() {
    QString path = QFileDialog::getSaveFileName(QtUtils::GetRootWidget(this), tr("Select path to input profile ini"),
                                                QString(), tr(INPUT_PROFILE_FILTER));
    if (path.isEmpty())
    {
      QMessageBox::critical(QtUtils::GetRootWidget(this), tr("Error"),
                            tr("No path selected, input profile was not saved."));
      return;
    }

    m_host_interface->saveInputProfile(path);
  });

  if (!profile_names.empty())
    menu.addSeparator();

  for (const auto& [name, path] : profile_names)
  {
    QAction* action = menu.addAction(QString::fromStdString(name));
    QString path_qstr = QString::fromStdString(path);
    connect(action, &QAction::triggered, [this, path_qstr]() { m_host_interface->saveInputProfile(path_qstr); });
  }

  menu.exec(QCursor::pos());
}
