package umido.androidemu.mame;

import static android.view.KeyEvent.KEYCODE_BUTTON_L1;
import static android.view.KeyEvent.KEYCODE_BUTTON_SELECT;
import static android.view.KeyEvent.KEYCODE_BUTTON_START;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.AttributeSet;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceView;

import androidx.core.content.IntentCompat;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class EmulationSurfaceView extends SurfaceView {
    private boolean attractMode;
    private boolean mResetting;
    public EmulationSurfaceView(Context context) {
        super(context);
        mResetting = false;
    }

    public EmulationSurfaceView(Context context, AttributeSet attrs) {
        super(context, attrs);
        mResetting = false;
    }

    public EmulationSurfaceView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
        mResetting = false;
    }

    private boolean isDPadOrButtonEvent(KeyEvent event) {
        final int source = event.getSource();
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD ||
                (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if(attractMode){
            ((EmulationActivity) getContext()).finishAffinity();
            System.exit(0);
            return true;
        }
        if(event.getEventTime()-event.getDownTime() >= 5000){
            Log.d("EnumationSurfaceView", "Long press");
            if(keyCode == KEYCODE_BUTTON_START) { // P1 Start
                Log.d("EmulationSurfaceView", "shutdown app");
                //AndroidHostInterface.getInstance().stopEmulationThread();
                //AndroidHostInterface.getInstance().shutdown();
                ((EmulationActivity) getContext()).finishAffinity();
                System.exit(0);
                return true;
            }else if(!mResetting && keyCode == KEYCODE_BUTTON_L1) { // P2 Start
                Log.d("EmulationSurfaceView", "restart game");
                //AndroidHostInterface.getInstance().stopEmulationThread();
                Activity activity = (EmulationActivity)getContext();
                final PackageManager pm = activity.getPackageManager();
                final Intent intent = pm.getLaunchIntentForPackage(activity.getPackageName());
                activity.finishAffinity(); // Finishes all activities.
                Bundle extras = activity.getIntent().getExtras();
                for(String key : extras.keySet()) {
                    Object val =extras.get(key);
                    if(val instanceof Integer ||
                        val instanceof Short)
                        intent.putExtra(key, (int)val);
                    else if(val instanceof Boolean){
                        intent.putExtra(key, (boolean)val);
                    }
                }
                
                activity.startActivity(intent);    // Start the launch activity
                System.exit(0);
                //((EmulationActivity) getContext()).recreate();
                /*((EmulationActivity) getContext()).runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        AndroidHostInterface.getInstance().resetSystem();
                    }
                });*/
                mResetting = true;
                return true;
            }
        }
        if (isDPadOrButtonEvent(event) && event.getRepeatCount() == 0 &&
                handleControllerKey(event.getDeviceId(), keyCode, true)) {
            return true;
        }

        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (isDPadOrButtonEvent(event) && event.getRepeatCount() == 0 &&
                handleControllerKey(event.getDeviceId(), keyCode, false)) {
            return true;
        }

        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        final int source = event.getSource();
        if ((source & InputDevice.SOURCE_JOYSTICK) == 0)
            return super.onGenericMotionEvent(event);

        final int deviceId = event.getDeviceId();
        for (AxisMapping mapping : mControllerAxisMapping) {
            if (mapping.deviceId != deviceId)
                continue;

            final float axisValue = event.getAxisValue(mapping.deviceAxisOrButton);
            float emuValue;

            if (mapping.deviceMotionRange != null) {
                final float transformedValue = (axisValue - mapping.deviceMotionRange.getMin()) / mapping.deviceMotionRange.getRange();
                emuValue = (transformedValue * 2.0f) - 1.0f;
            } else {
                emuValue = axisValue;
            }
            Log.d("EmulationSurfaceView", String.format("axis %d value %f emuvalue %f", mapping.deviceAxisOrButton, axisValue, emuValue));

            if (mapping.axisMapping >= 0) {
                AndroidHostInterface.getInstance().setControllerAxisState(0, mapping.axisMapping, emuValue);
            }

            final float DEAD_ZONE = 0.25f;
            if (mapping.negativeButton >= 0) {
                AndroidHostInterface.getInstance().setControllerButtonState(0, mapping.negativeButton, (emuValue <= -DEAD_ZONE));
            }
            if (mapping.positiveButton >= 0) {
                AndroidHostInterface.getInstance().setControllerButtonState(0, mapping.positiveButton, (emuValue >= DEAD_ZONE));
            }
        }

        return true;
    }

    private class ButtonMapping {
        public ButtonMapping(int deviceId, int deviceButton, int controllerIndex, int button) {
            this.deviceId = deviceId;
            this.deviceAxisOrButton = deviceButton;
            this.controllerIndex = controllerIndex;
            this.buttonMapping = button;
        }

        public int deviceId;
        public int deviceAxisOrButton;
        public int controllerIndex;
        public int buttonMapping;
    }

    private class AxisMapping {
        public AxisMapping(int deviceId, int deviceAxis, InputDevice.MotionRange motionRange, int controllerIndex, int axis) {
            this.deviceId = deviceId;
            this.deviceAxisOrButton = deviceAxis;
            this.deviceMotionRange = motionRange;
            this.controllerIndex = controllerIndex;
            this.axisMapping = axis;
            this.positiveButton = -1;
            this.negativeButton = -1;
        }

        public AxisMapping(int deviceId, int deviceAxis, InputDevice.MotionRange motionRange, int controllerIndex, int positiveButton, int negativeButton) {
            this.deviceId = deviceId;
            this.deviceAxisOrButton = deviceAxis;
            this.deviceMotionRange = motionRange;
            this.controllerIndex = controllerIndex;
            this.axisMapping = -1;
            this.positiveButton = positiveButton;
            this.negativeButton = negativeButton;
        }

        public int deviceId;
        public int deviceAxisOrButton;
        public InputDevice.MotionRange deviceMotionRange;
        public int controllerIndex;
        public int axisMapping;
        public int positiveButton;
        public int negativeButton;
    }

    private ArrayList<ButtonMapping> mControllerKeyMapping;
    private ArrayList<AxisMapping> mControllerAxisMapping;

    public void setAttractMode(boolean attractMode){
        this.attractMode = attractMode;
    }
    private void addControllerKeyMapping(int deviceId, int keyCode, int controllerIndex, String controllerType, String buttonName) {
        int mapping = AndroidHostInterface.getControllerButtonCode(controllerType, buttonName);
        //Log.i("EmulationSurfaceView", String.format("Map %d to %d (%s)", keyCode, mapping,buttonName));
        if (mapping >= 0) {
            mControllerKeyMapping.add(new ButtonMapping(deviceId, keyCode, controllerIndex, mapping));
        }
    }

    private void addControllerAxisMapping(int deviceId, List<InputDevice.MotionRange> motionRanges, int axis, int controllerIndex, String controllerType, String axisName, String negativeButtonName, String positiveButtonName) {
        InputDevice.MotionRange range = null;
        for (InputDevice.MotionRange curRange : motionRanges) {
            if (curRange.getAxis() == axis) {
                range = curRange;
                break;
            }
        }
        if (range == null)
            return;

        if (axisName != null) {
            int mapping = AndroidHostInterface.getControllerAxisCode(controllerType, axisName);
            Log.i("EmulationSurfaceView", String.format("Map axis %d to %d (%s)", axis, mapping, axisName));
            if (mapping >= 0) {
                mControllerAxisMapping.add(new AxisMapping(deviceId, axis, range, controllerIndex, mapping));
                return;
            }
        }

        if (negativeButtonName != null && positiveButtonName != null) {
            final int negativeMapping = AndroidHostInterface.getControllerButtonCode(controllerType, negativeButtonName);
            final int positiveMapping = AndroidHostInterface.getControllerButtonCode(controllerType, positiveButtonName);
            Log.i("EmulationSurfaceView", String.format("Map axis %d to %d %d (button %s %s)", axis, negativeMapping, positiveMapping,
                    negativeButtonName, positiveButtonName));
            if (negativeMapping >= 0 && positiveMapping >= 0) {
                mControllerAxisMapping.add(new AxisMapping(deviceId, axis, range, controllerIndex, positiveMapping, negativeMapping));
            }
        }
    }

    private static boolean isJoystickDevice(int deviceId) {
        if (deviceId < 0)
            return false;

        final InputDevice dev = InputDevice.getDevice(deviceId);
        if (dev == null)
            return false;

        final int sources = dev.getSources();
        if ((sources & InputDevice.SOURCE_CLASS_JOYSTICK) != 0)
            return true;

        if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)
            return true;

        return (sources & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    public boolean initControllerMapping(String controllerType) {
        mControllerKeyMapping = new ArrayList<>();
        mControllerAxisMapping = new ArrayList<>();

        final int[] deviceIds = InputDevice.getDeviceIds();
        for (int deviceId : deviceIds) {
            if (!isJoystickDevice(deviceId))
                continue;

            InputDevice device = InputDevice.getDevice(deviceId);
            List<InputDevice.MotionRange> motionRanges = device.getMotionRanges();
            int controllerIndex = 0;

            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_DPAD_UP, controllerIndex, controllerType, "Up");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_DPAD_RIGHT, controllerIndex, controllerType, "Right");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_DPAD_DOWN, controllerIndex, controllerType, "Down");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_DPAD_LEFT, controllerIndex, controllerType, "Left");
            addControllerKeyMapping(deviceId, KEYCODE_BUTTON_L1, controllerIndex, controllerType, "L1");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_BUTTON_L2, controllerIndex, controllerType, "L2");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_BUTTON_SELECT, controllerIndex, controllerType, "Select");
            addControllerKeyMapping(deviceId, KEYCODE_BUTTON_START, controllerIndex, controllerType, "Start");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_BUTTON_Y, controllerIndex, controllerType, "Triangle");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_BUTTON_B, controllerIndex, controllerType, "Circle");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_BUTTON_A, controllerIndex, controllerType, "Cross");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_BUTTON_X, controllerIndex, controllerType, "Square");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_BUTTON_R1, controllerIndex, controllerType, "R1");
            addControllerKeyMapping(deviceId, KeyEvent.KEYCODE_BUTTON_R2, controllerIndex, controllerType, "R2");
            if (motionRanges != null) {
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_X, controllerIndex, controllerType, "LeftX", null, null);
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_Y, controllerIndex, controllerType, "LeftY", null, null);
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_RX, controllerIndex, controllerType, "RightX", null, null);
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_RY, controllerIndex, controllerType, "RightY", null, null);
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_Z, controllerIndex, controllerType, "RightX", null, null);
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_RZ, controllerIndex, controllerType, "RightY", null, null);
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_LTRIGGER, controllerIndex, controllerType, "L2", "L2", "L2");
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_RTRIGGER, controllerIndex, controllerType, "R2", "R2", "R2");
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_HAT_X, controllerIndex, controllerType, null, "Left", "Right");
                addControllerAxisMapping(deviceId, motionRanges, MotionEvent.AXIS_HAT_Y, controllerIndex, controllerType, null, "Up", "Down");
            }
        }

        return !mControllerKeyMapping.isEmpty() || !mControllerKeyMapping.isEmpty();
    }

    private boolean handleControllerKey(int deviceId, int keyCode, boolean pressed) {
        boolean result = false;
        for (ButtonMapping mapping : mControllerKeyMapping) {
            if (mapping.deviceId != deviceId || mapping.deviceAxisOrButton != keyCode)
                continue;

            AndroidHostInterface.getInstance().setControllerButtonState(0, mapping.buttonMapping, pressed);
            //Log.d("EmulationSurfaceView", String.format("handleControllerKey %d -> %d %d", keyCode, mapping.buttonMapping, pressed ? 1 : 0));
            result = true;
        }

        return result;
    }
}
