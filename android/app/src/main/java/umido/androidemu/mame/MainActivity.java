package umido.androidemu.mame;

import android.Manifest;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ListView;
import android.widget.PopupMenu;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.Toolbar;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.preference.PreferenceManager;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.ShortBuffer;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

public class MainActivity extends AppCompatActivity {
    private static final int REQUEST_EXTERNAL_STORAGE_PERMISSIONS = 1;
    private static final int REQUEST_ADD_DIRECTORY_TO_GAME_LIST = 2;
    private static final int REQUEST_IMPORT_BIOS_IMAGE = 3;
    private static final int REQUEST_START_FILE = 4;

    private static final int OFFSET_DIFFICULTY=0x12;
    private static final int OFFSET_TIMING=0x13;

    private GameList mGameList;
    private ListView mGameListView;
    private boolean mHasExternalStoragePermissions = false;

    private void setLanguage() {
        String language = PreferenceManager.getDefaultSharedPreferences(this).getString("Main/Language", "none");
        if (language == null || language.equals("none")) {
            return;
        }

        String[] parts = language.split("-");
        if (parts.length < 2)
            return;

        Locale locale = new Locale(parts[0], parts[1]);
        Locale.setDefault(locale);

        Resources res = getResources();
        Configuration config = res.getConfiguration();
        config.setLocale(locale);
        res.updateConfiguration(config, res.getDisplayMetrics());
    }

    private boolean shouldResumeStateByDefault() {
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
        return prefs.getBoolean("Main/SaveStateOnExit", false);
    }

    private static String getTitleString() {
        String scmVersion = AndroidHostInterface.getScmVersion();
        final int gitHashPos = scmVersion.indexOf("-g");
        if (gitHashPos > 0)
            scmVersion = scmVersion.substring(0, gitHashPos);

        return String.format("Arcade1Up %s", scmVersion);
    }
    private boolean sameVersion(String verFile){
        if(!new File(verFile).exists()) {
            Log.i("MainActivity",  "version file  " + verFile + " does not exist");
            return false;
        }
        try {
            FileReader rdr = new FileReader(verFile);
            BufferedReader bufferedReader = new BufferedReader(rdr);
            String line = bufferedReader.readLine();
            bufferedReader.close();
            rdr.close();
            Log.i("MainActivity", "Exising version: " + line +" Installed version: " + BuildConfig.VERSION_NAME);
            if (line.equals(BuildConfig.VERSION_NAME)){
                return true;
            }
        }catch (Exception ex){
            Log.e("MainActivity", "Error reading version", ex);
        }
        return false;
    }

    private void updateVersion(String verFile) {
        try {
            FileWriter fos = new FileWriter(verFile);
            BufferedWriter bufferedWriter = new BufferedWriter(fos);
            bufferedWriter.write(BuildConfig.VERSION_NAME);
            bufferedWriter.close();
            fos.close();
        } catch (Exception ex) {
            Log.e("MainActivity", "Error updating version", ex);
        }
    }

    private void deleteRecursive(File fileOrDirectory) {
            if(!fileOrDirectory.exists())
                return;
            if (fileOrDirectory.isDirectory())
                for (File child : fileOrDirectory.listFiles())
                    deleteRecursive(child);

            fileOrDirectory.delete();

    }
    private void copyInternalFiles(String baseFolder){
        if (!sameVersion(baseFolder + "version")) {
            Log.i("MainActivity", "Identified older version");
            deleteRecursive(new File(baseFolder));
            if (!new File(baseFolder + "bios").mkdirs())
                Log.e("MainActivity", "Failed to create " + baseFolder + "bios");
            if (!new File(baseFolder + "simpsons").mkdirs())
                Log.e("MainActivity", "Failed to create " + baseFolder + "simpsons");

            updateVersion(baseFolder + "version");

            // TODO: Your Simpsons Bowling ISO and BIOS here
//            copyResource(R.raw.bios, baseFolder + "bios/999a01.7e", false);
//            copyResource(R.raw.iso, baseFolder + "simpsons/arcade.iso", false);
//            copyResource(R.raw.flash0, baseFolder + "simpsons/flash0", false);
//            copyResource(R.raw.flash1, baseFolder + "simpsons/flash1", false);
//            copyResource(R.raw.flash2, baseFolder + "simpsons/flash2", false);
//            copyResource(R.raw.flash3, baseFolder + "simpsons/flash3", false);
        }
//        copyResource(R.raw.eeprom, baseFolder + "simpsons/eeprom", true);
    }

    private void copyResource(int resId, String path, boolean overwrite){
        if(!overwrite && new File(path).exists()) return;
        InputStream is = this.getResources().openRawResource(resId);
        try {
            OutputStream os = new FileOutputStream(new File(path));
            byte[] buffer = new byte[1024];
            int len;
            while((len = is.read(buffer, 0, buffer.length)) != -1){
                os.write(buffer, 0, len);
            }
            is.close();
            os.close();

        }catch (Exception ex){
            Log.e( "MainActivity","Error copying " + resId + " to " + path, ex);
        }
    }
    private void enableFullscreenImmersive() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                        View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                        View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                        View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                        View.SYSTEM_UI_FLAG_FULLSCREEN |
                        View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setLanguage();
        enableFullscreenImmersive();
        setContentView(R.layout.activity_main);
        /*Toolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);
        getSupportActionBar().setTitle(getTitleString());

        findViewById(R.id.fab_add_game_directory).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                startAddGameDirectory();
            }
        });
        findViewById(R.id.fab_resume).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                startEmulation(null, shouldResumeStateByDefault());
            }
        });

        // Set up game list view.
        mGameList = new GameList(this);
        mGameListView = findViewById(R.id.game_list_view);
        mGameListView.setAdapter(mGameList.getListViewAdapter());
        mGameListView.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                startEmulation(mGameList.getEntry(position).getPath(), shouldResumeStateByDefault());
            }
        });
        mGameListView.setOnItemLongClickListener(new AdapterView.OnItemLongClickListener() {
            @Override
            public boolean onItemLongClick(AdapterView<?> parent, View view, int position,
                                           long id) {
                PopupMenu menu = new PopupMenu(MainActivity.this, view,
                        Gravity.RIGHT | Gravity.TOP);
                menu.getMenuInflater().inflate(R.menu.menu_game_list_entry, menu.getMenu());
                menu.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
                    @Override
                    public boolean onMenuItemClick(MenuItem item) {
                        int id = item.getItemId();
                        if (id == R.id.game_list_entry_menu_start_game) {
                            startEmulation(mGameList.getEntry(position).getPath(), false);
                            return true;
                        } else if (id == R.id.game_list_entry_menu_resume_game) {
                            startEmulation(mGameList.getEntry(position).getPath(), true);
                            return true;
                        }
                        return false;
                    }
                });
                menu.show();
                return true;
            }
        });*/

        mHasExternalStoragePermissions = checkForExternalStoragePermissions();
        if (mHasExternalStoragePermissions)
            completeStartup();
    }

    private void completeStartup() {
        if (!AndroidHostInterface.hasInstance() && !AndroidHostInterface.createInstance(this)) {
            Log.i("MainActivity", "Failed to create host interface");
            throw new RuntimeException("Failed to create host interface");
        }
        //ComGameList.refresh(false, false, this);
        startEmulation(null, false);
    }

    private void startAddGameDirectory() {
        if (!checkForExternalStoragePermissions())
            return;

        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addCategory(Intent.CATEGORY_DEFAULT);
        i.putExtra(Intent.EXTRA_LOCAL_ONLY, true);
        startActivityForResult(Intent.createChooser(i, getString(R.string.main_activity_choose_directory)),
                REQUEST_ADD_DIRECTORY_TO_GAME_LIST);
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        // Inflate the menu; this adds items to the action bar if it is present.
        getMenuInflater().inflate(R.menu.menu_main, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        // Handle action bar item clicks here. The action bar will
        // automatically handle clicks on the Home/Up button, so long
        // as you specify a parent activity in AndroidManifest.xml.
        int id = item.getItemId();

        //noinspection SimplifiableIfStatement
        if (id == R.id.action_resume) {
            startEmulation(null, true);
        } else if (id == R.id.action_start_bios) {
            startEmulation(null, false);
        } else if (id == R.id.action_start_file) {
            startStartFile();
        } else if (id == R.id.action_add_game_directory) {
            startAddGameDirectory();
        } else if (id == R.id.action_scan_for_new_games) {
            mGameList.refresh(false, false, this);
        } else if (id == R.id.action_rescan_all_games) {
            mGameList.refresh(true, true, this);
        } else if (id == R.id.action_import_bios) {
            importBIOSImage();
        } else if (id == R.id.action_settings) {
            Intent intent = new Intent(this, SettingsActivity.class);
            startActivity(intent);
            return true;
        } else if (id == R.id.action_show_version) {
            showVersion();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    private String getPathFromUri(Uri uri) {
        String path = FileUtil.getFullPathFromUri(uri, this);
        if (path.length() < 5) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.main_activity_error)
                    .setMessage(R.string.main_activity_get_path_from_file_error)
                    .setPositiveButton(R.string.main_activity_ok, (dialog, button) -> {
                    })
                    .create()
                    .show();
            return null;
        }

        return path;
    }

    private String getPathFromTreeUri(Uri treeUri) {
        String path = FileUtil.getFullPathFromTreeUri(treeUri, this);
        if (path.length() < 5) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.main_activity_error)
                    .setMessage(R.string.main_activity_get_path_from_directory_error)
                    .setPositiveButton(R.string.main_activity_ok, (dialog, button) -> {
                    })
                    .create()
                    .show();
            return null;
        }

        return path;
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        switch (requestCode) {
            case REQUEST_ADD_DIRECTORY_TO_GAME_LIST: {
                if (resultCode != RESULT_OK)
                    return;

                String path = getPathFromTreeUri(data.getData());
                if (path == null)
                    return;

                SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
                Set<String> currentValues = prefs.getStringSet("GameList/RecursivePaths", null);
                if (currentValues == null)
                    currentValues = new HashSet<String>();

                currentValues.add(path);
                SharedPreferences.Editor editor = prefs.edit();
                editor.putStringSet("GameList/RecursivePaths", currentValues);
                editor.apply();
                Log.i("MainActivity", "Added path '" + path + "' to game list search directories");
                mGameList.refresh(false, false, this);
            }
            break;

            case REQUEST_IMPORT_BIOS_IMAGE: {
                if (resultCode != RESULT_OK)
                    return;

                onImportBIOSImageResult(data.getData());
            }
            break;

            case REQUEST_START_FILE: {
                if (resultCode != RESULT_OK)
                    return;

                String path = getPathFromUri(data.getData());
                if (path == null)
                    return;

                startEmulation(path, shouldResumeStateByDefault());
            }
            break;
        }
    }

    private boolean checkForExternalStoragePermissions() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE) ==
                PackageManager.PERMISSION_GRANTED &&
                ContextCompat
                        .checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) ==
                        PackageManager.PERMISSION_GRANTED) {
            return true;
        }

        ActivityCompat.requestPermissions(this,
                new String[]{Manifest.permission.READ_EXTERNAL_STORAGE,
                        Manifest.permission.WRITE_EXTERNAL_STORAGE},
                REQUEST_EXTERNAL_STORAGE_PERMISSIONS);
        return false;
    }

    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        // check that all were successful
        for (int i = 0; i < grantResults.length; i++) {
            if (grantResults[i] == PackageManager.PERMISSION_GRANTED) {
                if (!mHasExternalStoragePermissions) {
                    mHasExternalStoragePermissions = true;
                    completeStartup();
                }
            } else {
                Toast.makeText(this,
                        R.string.main_activity_external_storage_permissions_error,
                        Toast.LENGTH_LONG);
                finish();
            }
        }
    }

    private void patchEeprom(String eeprom, short difficulty, short timing){
        try{
            FileInputStream fis = new FileInputStream(eeprom);
            byte[] buf = new byte[64*2];
            int read = fis.read(buf);
            fis.close();
            if(read == buf.length) {
                Log.i("MainActivity", "Patching difficulty: " + difficulty + " timing: " + timing);
                ByteBuffer bb = ByteBuffer.wrap(buf);
                short[] data = new short[64];
                bb = bb.order(ByteOrder.LITTLE_ENDIAN);
                bb.asShortBuffer().get(data);
                data[OFFSET_DIFFICULTY] =  (short)(difficulty - 1);
                data[OFFSET_TIMING] = timing;
                data[8] = 0x7f;
                short crc = (short) getCRC(data);
                if (data[0] != crc) {
                    data[0] = crc;
                    bb.clear();
                    for (short s : data) bb.putShort(s);

                    FileOutputStream fos = new FileOutputStream(eeprom);
                    fos.write(bb.array());
                    fos.close();
                }
            }else{
                Log.i("MainActivity", "Invalid size for eeprom " + read);
            }
        }catch (Exception ex){
            Log.e("MainActivity", "Error patching eeprom", ex);
        }
    }

    private short getCRC(short[] data){
        short crc = 0;
        for(int i =1; i < data.length; i++){
            crc += data[i];
        }
        return crc;
    }

    private boolean startEmulation(String bootPath, boolean resumeState) {
        String baseFolder = "/sdcard/arcade1up/";

        //deleteRecursive(new File(baseFolder));

        copyInternalFiles(baseFolder);
        if (!doBIOSCheck())
            return false;
        //if(bootPath == null){
            bootPath= baseFolder + "simpsons/arcade.iso";//
            //bootPath="/sdcard/duckstation/Spyro the Dragon (USA).cue";
            //resumeState = false;
        //}
        //if(getIntent().hasExtra("difficulty")){
            short difficulty =(short) getIntent().getIntExtra("difficulty", 4);
            short timing = (short)getIntent().getIntExtra("timing", 1);
            patchEeprom(baseFolder+"simpsons/eeprom", difficulty, timing);
        //}
        Intent intent = new Intent(this, EmulationActivity.class);
        intent.putExtra("bootPath", bootPath);
        intent.putExtra("resumeState", false);
        intent.putExtra("difficulty", (int)difficulty);
        intent.putExtra("timing", (int)timing);
        intent.putExtra("attract", getIntent().getBooleanExtra("attract", false));

        startActivity(intent);
        return true;
    }

    private void startStartFile() {
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType("*/*");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(Intent.createChooser(intent, getString(R.string.main_activity_choose_disc_image)), REQUEST_START_FILE);
        //startEmulation("/sdcard/duckstation/memcards/Spyro the Dragon (USA).iso", false);
    }

    private boolean doBIOSCheck() {
        //if (AndroidHostInterface.getInstance().hasAnyBIOSImages())
            return true;

        /*new AlertDialog.Builder(this)
                .setTitle(R.string.main_activity_missing_bios_image)
                .setMessage(R.string.main_activity_missing_bios_image_prompt)
                .setPositiveButton(R.string.main_activity_yes, (dialog, button) -> importBIOSImage())
                .setNegativeButton(R.string.main_activity_no, (dialog, button) -> {
                })
                .create()
                .show();

        return false;*/
    }

    private void importBIOSImage() {
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType("*/*");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(Intent.createChooser(intent, getString(R.string.main_activity_choose_bios_image)), REQUEST_IMPORT_BIOS_IMAGE);
    }

    private void onImportBIOSImageResult(Uri uri) {
        // This should really be 512K but just in case we wanted to support the other BIOSes in the future...
        final int MAX_BIOS_SIZE = 2 * 1024 * 1024;

        InputStream stream = null;
        try {
            stream = getContentResolver().openInputStream(uri);
        } catch (FileNotFoundException e) {
            Toast.makeText(this, R.string.main_activity_failed_to_open_bios_image, Toast.LENGTH_LONG);
            return;
        }

        ByteArrayOutputStream os = new ByteArrayOutputStream();
        try {
            byte[] buffer = new byte[512 * 1024];
            int len;
            while ((len = stream.read(buffer)) > 0) {
                os.write(buffer, 0, len);
                if (os.size() > MAX_BIOS_SIZE) {
                    throw new IOException(getString(R.string.main_activity_bios_image_too_large));
                }
            }
        } catch (IOException e) {
            new AlertDialog.Builder(this)
                    .setMessage(getString(R.string.main_activity_failed_to_read_bios_image_prefix) + e.getMessage())
                    .setPositiveButton(R.string.main_activity_ok, (dialog, button) -> {
                    })
                    .create()
                    .show();
            return;
        }

        String importResult = AndroidHostInterface.getInstance().importBIOSImage(os.toByteArray());
        String message = (importResult == null) ? getString(R.string.main_activity_invalid_error) : ("BIOS '" + importResult + "' imported.");

        new AlertDialog.Builder(this)
                .setMessage(message)
                .setPositiveButton(R.string.main_activity_ok, (dialog, button) -> {
                })
                .create()
                .show();
    }

    private void showVersion() {
        final String message = AndroidHostInterface.getFullScmVersion();
        new AlertDialog.Builder(this)
                .setTitle(R.string.main_activity_show_version_title)
                .setMessage(message)
                .setPositiveButton(R.string.main_activity_ok, (dialog, button) -> {
                })
                .setNeutralButton(R.string.main_activity_copy, (dialog, button) -> {
                    ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                    if (clipboard != null)
                        clipboard.setPrimaryClip(ClipData.newPlainText(getString(R.string.main_activity_show_version_title), message));
                })
                .create()
                .show();
    }
}
