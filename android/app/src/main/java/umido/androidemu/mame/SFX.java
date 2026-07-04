package umido.androidemu.mame;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.media.AudioManager;
import android.media.MediaPlayer;
import android.media.SoundPool;
import android.util.Log;

//import com.dpdev.rf2.ui.preferences.PreferenceUtil;

import java.io.FileInputStream;
import java.util.HashMap;

public class SFX {
    public static final int SFX_CANCEL = 1;
    public static final int SFX_QUIT = 2;
    public static final int SFX_CURSOR = 3;
    public static final int SFX_DECIDE = 4;
    public static final int SFX_LOAD = 5;

    public static final int SFX_EASTER_EGG1 = 6;

    private final static String TAG = "SFX";
    //public final static String SFX_SD_PATH =  "/SFX";

    private static SoundPool mSoundPool = null;
    private static HashMap<Integer, SfxInfo> mSoundsMap;
    //	private static boolean mSfxLoaded = false;
    private static boolean mSfxEnabled = true;
    private static long mStreamBlockTime = 0;
    private static boolean mSFXNotified = true;

    private static class SfxInfo
    {
        int streamID;
        int duration;
        SfxInfo(int id, int dur)
        {
            streamID = id;
            duration = dur;
//			if(RetronConfig.DEBUG) Log.i(TAG, "SfxInfo: " + duration);
        }
    }

    // load from resource
    private static SfxInfo getSfxInfo(Context context, int resId)
    {
        int duration = -1;
        MediaPlayer mp = new MediaPlayer();
        try
        {
            AssetFileDescriptor afd = context.getResources().openRawResourceFd(resId);
            mp.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
            mp.prepare();
            duration = mp.getDuration();
            afd.close();
        } catch (Exception e) {
            return null;
        } finally {
            mp.reset();
            mp.release();
            mp = null;
        }

        int streamId = mSoundPool.load(context, resId, 1);
        return new SfxInfo(streamId, duration);
    }

    // load from file on SD
    private static SfxInfo getSfxInfo(String f)
    {
        int duration = -1;
        MediaPlayer mp = new MediaPlayer();
        try
        {
            FileInputStream fs = new FileInputStream(f);
            mp.setDataSource(fs.getFD());
            mp.prepare();
            duration = mp.getDuration();
            fs.close();
        } catch (Exception e) {
            return null;
        } finally {
            mp.reset();
            mp.release();
            mp = null;
        }

        int streamId = mSoundPool.load(f, 1);
        return new SfxInfo(streamId, duration);
    }

    public synchronized static void InitSFX(Context context)
    {
        if (mSoundPool != null)
        {
            return;
        }

        //SharedPreferences sharedPref = PreferenceManager.getDefaultSharedPreferences(context);
        //SharedPreferences prefs = RF2Application.getInstance().getSharedPref();
        mSfxEnabled = true;//PreferenceUtil.isGUISoundEnabled();
        Log.i("SFX", "GUI sound - " + (mSfxEnabled? "enabled" : "disabled"));
//		mSfxLoaded = false;
        mSoundPool = new SoundPool(32, AudioManager.STREAM_MUSIC, 0);
        mSoundsMap = new HashMap<Integer, SfxInfo>();

        //mSoundsMap.put(SFX_CANCEL, getSfxInfo(context, R.raw.cancel));
        mSoundsMap.put(SFX_QUIT, getSfxInfo(context, R.raw.quit_game));
        mSoundsMap.put(SFX_CURSOR, getSfxInfo(context, R.raw.selection));
        mSoundsMap.put(SFX_DECIDE, getSfxInfo(context, R.raw.confirm));
        //mSoundsMap.put(SFX_LOAD, getSfxInfo(context, R.raw.load));

        //mSoundsMap.put(SFX_EASTER_EGG1, getSfxInfo(context, R.raw.horse_neigh));
    }

    public static void enableSFX(boolean enabled)
    {
        mSfxEnabled = enabled;
    }

    public synchronized static void notifySFX()
    {
        mSFXNotified = true;
    }

    public synchronized static void blockForDuration(int blockTime)
    {
        mStreamBlockTime = System.currentTimeMillis() + blockTime;
    }

    public static void playCancel() {
        long startTime = System.currentTimeMillis();
        playSound(SFX_CANCEL);
        mStreamBlockTime = startTime + (mSoundsMap.get(SFX_CANCEL).duration / 3);
    }

    public static void playClick() {
        playSound(SFX_QUIT);
    }

    public static void playCursor() {
        playSound(SFX_CURSOR);
    }

    public static void playDecide() {
        playSound(SFX_DECIDE);
    }

    public static void playLoad() {
        long startTime = System.currentTimeMillis();
        playSound(SFX_LOAD);
        mStreamBlockTime = startTime + (mSoundsMap.get(SFX_LOAD).duration / 3);
    }

    public synchronized static void playSound(int sound) {
        Log.i("SFX", "Play sound " + sound);
        if(mSfxEnabled == false) {
            Log.i("SFX", "Playing sound SF Not enabled" );
            return;
        }
        if(!mSFXNotified)
        {
            Log.i("SFX", "Playing sound SF Not notified" );
            return;
        }
        mSFXNotified = false;
        if(mStreamBlockTime != 0 && System.currentTimeMillis() < mStreamBlockTime)
        {
            Log.i("SFX", "Playing before finishing previous, so canceling");
            return;
        }

        SfxInfo sfx = mSoundsMap.get(sound);
        Log.i("SFX", "Playing sound " + sound);
        mSoundPool.play(sfx.streamID, 1, 1, 1, 0, 1);
    }

}
