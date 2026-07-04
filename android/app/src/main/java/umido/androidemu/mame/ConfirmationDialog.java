package umido.androidemu.mame;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.widget.Button;

import androidx.annotation.NonNull;

public class ConfirmationDialog extends Dialog implements android.view.View.OnClickListener {
    private Activity mActivity;
    private Button yes;
    private Button no;
    public ConfirmationDialog(Activity activity){
        super(activity);
        mActivity = activity;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {

        super.onCreate(savedInstanceState);
        //requestWindowFeature(Window.FEATURE_NO_TITLE);
        setContentView(R.layout.quit_confirmation);
        yes = (Button) findViewById(R.id.btn_yes);
        no = (Button) findViewById(R.id.btn_no);
        yes.setOnClickListener(this);
        no.setOnClickListener(this);
        yes.setOnFocusChangeListener(new View.OnFocusChangeListener() {
            @Override
            public void onFocusChange(View v, boolean hasFocus) {
                if (hasFocus) {
                    SFX.notifySFX();
                    SFX.playCursor();
                }
            }
        });
        no.setOnFocusChangeListener(new View.OnFocusChangeListener() {
            @Override
            public void onFocusChange(View v, boolean hasFocus) {
                if (hasFocus) {
                    SFX.notifySFX();
                    SFX.playCursor();
                }
            }
        });
        getWindow().getDecorView().setBackgroundColor(Color.TRANSPARENT);

    }
    
    @Override
    public void onClick(View v) {
        switch (v.getId()) {
            case R.id.btn_yes:
                SFX.notifySFX();
                SFX.playDecide();
                mActivity.finishAffinity();
                System.exit(0);
                break;
            case R.id.btn_no:
                SFX.notifySFX();
                SFX.playDecide();
                dismiss();
                if(AndroidHostInterface.getInstance().isEmulationThreadPaused())
                    AndroidHostInterface.getInstance().pauseEmulationThread(false);
                break;
            default:
                break;
        }
        //dismiss();
    }
}
