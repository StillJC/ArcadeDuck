#include "inputbindingwidgets.h"
#include "common/bitutils.h"
#include "common/string_util.h"
#include "core/settings.h"
#include "frontend-common/controller_interface.h"
#include "inputbindingdialog.h"
#include "inputbindingmonitor.h"
#include "qthostinterface.h"
#include "qtutils.h"
#if defined(_WIN32)
#include "common/windows_headers.h"
#include <cfgmgr32.h>
#include <setupapi.h>
#endif
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <cmath>

#if defined(_WIN32)
static bool IsGenericRawMouseDeviceNameForBinding(const QString& name)
{
  return name.isEmpty() || name == QStringLiteral("HID-compliant mouse") ||
         name == QStringLiteral("USB Input Device") || name == QStringLiteral("USB Composite Device") ||
         name == QStringLiteral("USB Root Hub (USB 3.0)") || name == QStringLiteral("Generic USB Hub") ||
         name.contains(QStringLiteral("Hub"), Qt::CaseInsensitive) ||
         name == QStringLiteral("HID-compliant vendor-defined device") ||
         name == QStringLiteral("HID-compliant consumer control device") ||
         name == QStringLiteral("HID-compliant system controller") || name == QStringLiteral("HID Keyboard Device");
}

static QString GetSetupAPIDevicePropertyForBinding(HDEVINFO device_info_set, SP_DEVINFO_DATA* device_info_data,
                                                   DWORD property)
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

static QString GetRawMouseVidPidTokenForBinding(const QString& device_name)
{
  const qsizetype vid_pos = device_name.indexOf(QStringLiteral("VID_"), 0, Qt::CaseInsensitive);
  const qsizetype pid_pos = device_name.indexOf(QStringLiteral("PID_"), 0, Qt::CaseInsensitive);

  if (vid_pos < 0 || pid_pos < 0)
    return {};

  return QStringLiteral("%1&%2").arg(device_name.mid(vid_pos, 8).toUpper(), device_name.mid(pid_pos, 8).toUpper());
}

static QString GetRawMouseSiblingDeviceDisplayNameForBinding(const QString& device_name)
{
  const QString vid_pid_token = GetRawMouseVidPidTokenForBinding(device_name);
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
                          static_cast<ULONG>(sizeof(instance_id_buffer) / sizeof(instance_id_buffer[0])),
                          0) != CR_SUCCESS)
    {
      continue;
    }

    const QString instance_id = QString::fromWCharArray(instance_id_buffer).toUpper();
    if (!instance_id.contains(vid_pid_token))
      continue;

    QString display_name = GetSetupAPIDevicePropertyForBinding(device_info_set, &device_info_data, SPDRP_FRIENDLYNAME);

    if (display_name.isEmpty())
      display_name = GetSetupAPIDevicePropertyForBinding(device_info_set, &device_info_data, SPDRP_DEVICEDESC);

    if (!IsGenericRawMouseDeviceNameForBinding(display_name))
    {
      best_display_name = display_name;
      break;
    }
  }

  SetupDiDestroyDeviceInfoList(device_info_set);
  return best_display_name;
}

static QString GetRawMouseSetupAPIDisplayNameForBinding(const QString& device_name)
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
        display_name = GetSetupAPIDevicePropertyForBinding(device_info_set, &device_info_data, SPDRP_FRIENDLYNAME);

        if (display_name.isEmpty())
          display_name = GetSetupAPIDevicePropertyForBinding(device_info_set, &device_info_data, SPDRP_DEVICEDESC);

        if (IsGenericRawMouseDeviceNameForBinding(display_name))
        {
          if (const QString sibling_display_name = GetRawMouseSiblingDeviceDisplayNameForBinding(device_name);
              !sibling_display_name.isEmpty())
          {
            display_name = sibling_display_name;
          }
        }
      }
    }
  }

  SetupDiDestroyDeviceInfoList(device_info_set);

  if (IsGenericRawMouseDeviceNameForBinding(display_name))
    return {};

  return display_name;
}

static QString GetRawMouseDisplayNameForBinding(const QString& device_name, u32 index)
{
  if (const QString setupapi_name = GetRawMouseSetupAPIDisplayNameForBinding(device_name); !setupapi_name.isEmpty())
    return setupapi_name;

  const qsizetype vid_pos = device_name.indexOf(QStringLiteral("VID_"), 0, Qt::CaseInsensitive);
  const qsizetype pid_pos = device_name.indexOf(QStringLiteral("PID_"), 0, Qt::CaseInsensitive);

  if (vid_pos >= 0 && pid_pos >= 0)
    return QStringLiteral("Wireless USB Mouse %1").arg(index + 1);

  return QStringLiteral("Raw Mouse %1").arg(index + 1);
}
#endif

InputBindingWidget::InputBindingWidget(QtHostInterface* host_interface, std::string section_name, std::string key_name,
                                       QWidget* parent)
  : QPushButton(parent), m_host_interface(host_interface), m_section_name(std::move(section_name)),
    m_key_name(std::move(key_name))
{
  m_bindings = m_host_interface->GetSettingStringList(m_section_name.c_str(), m_key_name.c_str());
  updateText();

  setMinimumWidth(150);
  setMaximumWidth(150);

  connect(this, &QPushButton::clicked, this, &InputBindingWidget::onClicked);
}

InputBindingWidget::~InputBindingWidget()
{
  Q_ASSERT(!isListeningForInput());
}

void InputBindingWidget::updateText()
{
  if (m_bindings.empty())
  {
    setText(QString());
    return;
  }

  if (m_bindings.size() > 1)
  {
    setText(tr("%n bindings", "", static_cast<int>(m_bindings.size())));
    return;
  }

  const std::string& binding = m_bindings[0];

#if defined(_WIN32)
  if (binding.rfind("Mouse/Button", 0) == 0)
  {
    const int button_id = std::atoi(binding.c_str() + std::strlen("Mouse/Button"));

    int controller_number = 0;
    int raw_button_number = 0;

    if (button_id >= 101 && button_id <= 105)
    {
      controller_number = 1;
      raw_button_number = button_id - 100;
    }
    else if (button_id >= 201 && button_id <= 205)
    {
      controller_number = 2;
      raw_button_number = button_id - 200;
    }

    if (controller_number != 0 && raw_button_number != 0)
    {
      const std::string controller_section = StringUtil::StdStringFromFormat("Controller%d", controller_number);
      std::string raw_device_setting =
        m_host_interface->GetStringSettingValue(controller_section.c_str(), "LightgunDevice", "");

      static constexpr char raw_mouse_prefix[] = "RawMouse:";
      if (raw_device_setting.rfind(raw_mouse_prefix, 0) == 0)
      {
        raw_device_setting.erase(0, std::strlen(raw_mouse_prefix));

        const QString raw_device_name = QString::fromStdString(raw_device_setting);
        const QString raw_display_name =
          GetRawMouseDisplayNameForBinding(raw_device_name, static_cast<u32>(controller_number - 1));

        setText(QStringLiteral("%1 Button %2").arg(raw_display_name).arg(raw_button_number));
        return;
      }
    }
  }
#endif

  setText(QString::fromStdString(binding));
}

void InputBindingWidget::bindToControllerAxis(int controller_index, int axis_index, bool inverted,
                                              std::optional<bool> half_axis_positive)
{
  const char* invert_char = inverted ? "-" : "";
  const char* sign_char = "";
  if (half_axis_positive)
  {
    sign_char = *half_axis_positive ? "+" : "-";
  }

  m_new_binding_value =
    StringUtil::StdStringFromFormat("Controller%d/%sAxis%d%s", controller_index, sign_char, axis_index, invert_char);
  setNewBinding();
  stopListeningForInput();
}

void InputBindingWidget::bindToControllerButton(int controller_index, int button_index)
{
  m_new_binding_value = StringUtil::StdStringFromFormat("Controller%d/Button%d", controller_index, button_index);
  setNewBinding();
  stopListeningForInput();
}

void InputBindingWidget::bindToControllerHat(int controller_index, int hat_index, const QString& hat_direction)
{
  m_new_binding_value = StringUtil::StdStringFromFormat("Controller%d/Hat%d %s", controller_index, hat_index,
                                                        hat_direction.toLatin1().constData());
  setNewBinding();
  stopListeningForInput();
}

void InputBindingWidget::beginRebindAll()
{
  m_is_binding_all = true;
  if (isListeningForInput())
    stopListeningForInput();

  startListeningForInput(TIMEOUT_FOR_ALL_BINDING);
}

bool InputBindingWidget::eventFilter(QObject* watched, QEvent* event)
{
  const QEvent::Type event_type = event->type();

  // if the key is being released, set the input
  if (event_type == QEvent::KeyRelease)
  {
    setNewBinding();
    stopListeningForInput();
    return true;
  }
  else if (event_type == QEvent::KeyPress)
  {
    const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
    const QString binding(QtUtils::KeyEventToString(key_event->key(), key_event->modifiers()));
    if (!binding.isEmpty())
      m_new_binding_value = QStringLiteral("Keyboard/%1").arg(binding).toStdString();

    return true;
  }
  else if (event_type == QEvent::MouseButtonRelease)
  {
    const u32 button_mask = static_cast<u32>(static_cast<const QMouseEvent*>(event)->button());
    const u32 button_index = (button_mask == 0u) ? 0 : CountTrailingZeros(button_mask);
    if (std::optional<std::string> raw_binding =
          m_host_interface->GetLastRawLightgunButtonBindingForController(m_section_name))
    {
      m_new_binding_value = std::move(raw_binding.value());
    }
    else
    {
      m_new_binding_value = StringUtil::StdStringFromFormat("Mouse/Button%d", button_index + 1);
    }
    setNewBinding();
    stopListeningForInput();
    return true;
  }

  if (event_type == QEvent::MouseButtonPress || event_type == QEvent::MouseButtonDblClick)
  {
    return true;
  }

  return false;
}

bool InputBindingWidget::event(QEvent* event)
{
  if (event->type() == QEvent::MouseButtonRelease)
  {
    QMouseEvent* mev = static_cast<QMouseEvent*>(event);
    if (mev->button() == Qt::LeftButton && mev->modifiers() & Qt::ShiftModifier)
    {
      openDialog();
      return false;
    }
  }

  return QPushButton::event(event);
}

void InputBindingWidget::mouseReleaseEvent(QMouseEvent* e)
{
  if (e->button() == Qt::RightButton)
  {
    clearBinding();
    return;
  }

  QPushButton::mouseReleaseEvent(e);
}

void InputBindingWidget::setNewBinding()
{
  if (m_new_binding_value.empty())
    return;

  m_host_interface->SetStringSettingValue(m_section_name.c_str(), m_key_name.c_str(), m_new_binding_value.c_str());
  m_host_interface->updateInputMap();

  m_bindings.clear();
  m_bindings.push_back(std::move(m_new_binding_value));
}

void InputBindingWidget::clearBinding()
{
  m_bindings.clear();
  m_host_interface->RemoveSettingValue(m_section_name.c_str(), m_key_name.c_str());
  m_host_interface->updateInputMap();
  updateText();
}

void InputBindingWidget::reloadBinding()
{
  m_bindings = m_host_interface->GetSettingStringList(m_section_name.c_str(), m_key_name.c_str());
  updateText();
}

void InputBindingWidget::onClicked()
{
  if (m_bindings.size() > 1)
  {
    openDialog();
    return;
  }

  if (isListeningForInput())
    stopListeningForInput();

  startListeningForInput(TIMEOUT_FOR_SINGLE_BINDING);
}

void InputBindingWidget::onInputListenTimerTimeout()
{
  m_input_listen_remaining_seconds--;
  if (m_input_listen_remaining_seconds == 0)
  {
    stopListeningForInput();
    return;
  }

  setText(tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds));
}

void InputBindingWidget::startListeningForInput(u32 timeout_in_seconds)
{
  m_input_listen_timer = new QTimer(this);
  m_input_listen_timer->setSingleShot(false);
  m_input_listen_timer->start(1000);

  m_input_listen_timer->connect(m_input_listen_timer, &QTimer::timeout, this,
                                &InputBindingWidget::onInputListenTimerTimeout);
  m_input_listen_remaining_seconds = timeout_in_seconds;
  setText(tr("Push Button/Axis... [%1]").arg(m_input_listen_remaining_seconds));

  installEventFilter(this);
  grabKeyboard();
  grabMouse();
}

void InputBindingWidget::stopListeningForInput()
{
  updateText();
  delete m_input_listen_timer;
  m_input_listen_timer = nullptr;

  releaseMouse();
  releaseKeyboard();
  removeEventFilter(this);

  if (m_is_binding_all && m_next_widget)
    m_next_widget->beginRebindAll();
  m_is_binding_all = false;
}

void InputBindingWidget::openDialog() {}

InputButtonBindingWidget::InputButtonBindingWidget(QtHostInterface* host_interface, std::string section_name,
                                                   std::string key_name, QWidget* parent)
  : InputBindingWidget(host_interface, std::move(section_name), std::move(key_name), parent)
{
}

InputButtonBindingWidget::~InputButtonBindingWidget()
{
  if (isListeningForInput())
    InputButtonBindingWidget::stopListeningForInput();
}

void InputButtonBindingWidget::hookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->SetHook(InputButtonBindingMonitor(this));
}

void InputButtonBindingWidget::unhookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->ClearHook();
}

void InputButtonBindingWidget::startListeningForInput(u32 timeout_in_seconds)
{
  InputBindingWidget::startListeningForInput(timeout_in_seconds);
  hookControllerInput();
}

void InputButtonBindingWidget::stopListeningForInput()
{
  unhookControllerInput();
  InputBindingWidget::stopListeningForInput();
}

void InputButtonBindingWidget::openDialog()
{
  InputButtonBindingDialog binding_dialog(m_host_interface, m_section_name, m_key_name, m_bindings,
                                          QtUtils::GetRootWidget(this));
  binding_dialog.exec();
  reloadBinding();
}

InputAxisBindingWidget::InputAxisBindingWidget(QtHostInterface* host_interface, std::string section_name,
                                               std::string key_name, Controller::AxisType axis_type, QWidget* parent)
  : InputBindingWidget(host_interface, std::move(section_name), std::move(key_name), parent), m_axis_type(axis_type)
{
}

InputAxisBindingWidget::~InputAxisBindingWidget()
{
  if (isListeningForInput())
    InputAxisBindingWidget::stopListeningForInput();
}

void InputAxisBindingWidget::hookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->SetHook(InputAxisBindingMonitor(this, m_axis_type));
}

void InputAxisBindingWidget::unhookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->ClearHook();
}

bool InputAxisBindingWidget::eventFilter(QObject* watched, QEvent* event)
{
  if (m_axis_type != Controller::AxisType::Half)
  {
    const QEvent::Type event_type = event->type();

    if (event_type == QEvent::KeyRelease || event_type == QEvent::KeyPress || event_type == QEvent::MouseButtonRelease)
    {
      return true;
    }
  }

  return InputBindingWidget::eventFilter(watched, event);
}

void InputAxisBindingWidget::startListeningForInput(u32 timeout_in_seconds)
{
  InputBindingWidget::startListeningForInput(timeout_in_seconds);
  hookControllerInput();
}

void InputAxisBindingWidget::stopListeningForInput()
{
  unhookControllerInput();
  InputBindingWidget::stopListeningForInput();
}

void InputAxisBindingWidget::openDialog()
{
  InputAxisBindingDialog binding_dialog(m_host_interface, m_section_name, m_key_name, m_bindings, m_axis_type,
                                        QtUtils::GetRootWidget(this));
  binding_dialog.exec();
  reloadBinding();
}

InputRumbleBindingWidget::InputRumbleBindingWidget(QtHostInterface* host_interface, std::string section_name,
                                                   std::string key_name, QWidget* parent)
  : InputBindingWidget(host_interface, std::move(section_name), std::move(key_name), parent)
{
}

InputRumbleBindingWidget::~InputRumbleBindingWidget()
{
  if (isListeningForInput())
    InputRumbleBindingWidget::stopListeningForInput();
}

void InputRumbleBindingWidget::hookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->SetHook(InputRumbleBindingMonitor(this));
}

void InputRumbleBindingWidget::unhookControllerInput()
{
  ControllerInterface* controller_interface = m_host_interface->getControllerInterface();
  if (!controller_interface)
    return;

  controller_interface->ClearHook();
}

void InputRumbleBindingWidget::bindToControllerRumble(int controller_index)
{
  m_new_binding_value = StringUtil::StdStringFromFormat("Controller%d", controller_index);
  setNewBinding();
  stopListeningForInput();
}

void InputRumbleBindingWidget::startListeningForInput(u32 timeout_in_seconds)
{
  InputBindingWidget::startListeningForInput(timeout_in_seconds);
  hookControllerInput();
}

void InputRumbleBindingWidget::stopListeningForInput()
{
  unhookControllerInput();
  InputBindingWidget::stopListeningForInput();
}
