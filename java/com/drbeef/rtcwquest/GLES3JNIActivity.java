
package com.drbeef.rtcwquest;


import java.io.BufferedReader;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;

import android.media.AudioRecord;
import android.media.AudioTrack;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.RemoteException;
import android.provider.Settings;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

import com.drbeef.externalhapticsservice.HapticServiceClient;

import android.support.v4.app.ActivityCompat;
import android.support.v4.content.ContextCompat;

import static android.system.Os.setenv;

@SuppressLint("SdCardPath") public class GLES3JNIActivity extends Activity implements SurfaceHolder.Callback
{
	// Load the gles3jni library right away to make sure JNI_OnLoad() gets called as the very first thing.
	static
	{
		String manufacturer = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER.toLowerCase();
		String openXrHmd = manufacturer.contains("oculus") || manufacturer.contains("meta")
				? "meta"
				: manufacturer.contains("pico") ? "pico" : manufacturer;
		try {
			setenv("OPENXR_HMD", openXrHmd, true);
			System.loadLibrary("openxr_loader");
		} catch (Throwable t) {
			Log.w("RTCWQuest", "Unable to pre-load OpenXR loader", t);
		}
		System.loadLibrary( "rtcw_client" );
	}

	private HapticServiceClient externalHapticsServiceClient = null;

	private static final String TAG = "RTCWQuest";
	// Lifecycle diagnostics, same tag as the native side (adb logcat -s RTCWLifecycle:I)
	private static final String LIFE = "RTCWLifecycle";

	private static void life(String what)
	{
		Log.i( LIFE, "v1 java " + what + " thread=" + Thread.currentThread().getName() );
	}
	private static final String APPLICATION = "RTCWQuest";

	private boolean permissionsGranted = false;
	private static final int READ_EXTERNAL_STORAGE_PERMISSION_ID = 1;
	private static final int WRITE_EXTERNAL_STORAGE_PERMISSION_ID = 2;
	private static final int MANAGE_EXTERNAL_STORAGE_PERMISSION_ID = 3;

	String commandLineParams;

	private SurfaceView mView;
	private SurfaceHolder mSurfaceHolder;
	private long mNativeHandle;
	private PowerManager.WakeLock mWakeLock;

	// Main components
	protected static GLES3JNIActivity mSingleton;

	// Audio
	protected static AudioTrack mAudioTrack;
	protected static AudioRecord mAudioRecord;

	public static void initialize() {
		// The static nature of the singleton and Android quirkyness force us to initialize everything here
		// Otherwise, when exiting the app and returning to it, these variables *keep* their pre exit values
		mSingleton = null;
		mAudioTrack = null;
		mAudioRecord = null;
	}

	public void shutdown() {
		System.exit(0);
	}

	public void haptic_event(String event, int position, int flags, int intensity, float angle, float yHeight)  {

		if (externalHapticsServiceClient.hasService()) {
			try {
				externalHapticsServiceClient.getHapticsService().hapticEvent(APPLICATION, event, position, flags, intensity, angle, yHeight);
			}
			catch (RemoteException r)
			{
				Log.v(APPLICATION, r.toString());
			}
		}
	}

	public void haptic_updateevent(String event, int intensity, float angle) {

		if (externalHapticsServiceClient.hasService()) {
			try {
				externalHapticsServiceClient.getHapticsService().hapticUpdateEvent(APPLICATION, event, intensity, angle);
			}
			catch (RemoteException r)
			{
				Log.v(APPLICATION, r.toString());
			}
		}
	}

	public void haptic_stopevent(String event) {

		if (externalHapticsServiceClient.hasService()) {
			try {
				externalHapticsServiceClient.getHapticsService().hapticStopEvent(APPLICATION, event);
			}
			catch (RemoteException r)
			{
				Log.v(APPLICATION, r.toString());
			}
		}
	}

	public void haptic_endframe() {

		if (externalHapticsServiceClient.hasService()) {
			try {
				externalHapticsServiceClient.getHapticsService().hapticFrameTick();
			}
			catch (RemoteException r)
			{
				Log.v(APPLICATION, r.toString());
			}
		}
	}

	public void haptic_enable() {

		if (externalHapticsServiceClient.hasService()) {
			try {
				externalHapticsServiceClient.getHapticsService().hapticEnable();
			}
			catch (RemoteException r)
			{
				Log.v(APPLICATION, r.toString());
			}
		}
	}

	public void haptic_disable() {

		if (externalHapticsServiceClient.hasService()) {
			try {
				externalHapticsServiceClient.getHapticsService().hapticDisable();
			}
			catch (RemoteException r)
			{
				Log.v(APPLICATION, r.toString());
			}
		}
	}

	@Override protected void onCreate( Bundle icicle )
	{
		Log.v( TAG, "----------------------------------------------------------------" );
		Log.v( TAG, "GLES3JNIActivity::onCreate()" );
		super.onCreate( icicle );

		GLES3JNIActivity.initialize();

		// So we can call stuff from static callbacks
		mSingleton = this;


		mView = new SurfaceView( this );
		setContentView( mView );
		mView.getHolder().addCallback( this );

		// Force the screen to stay on, rather than letting it dim and shut off
		// while the user is watching a movie.
		getWindow().addFlags( WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON );

		// Force screen brightness to stay at maximum
		WindowManager.LayoutParams params = getWindow().getAttributes();
		params.screenBrightness = 1.0f;
		getWindow().setAttributes( params );

		checkPermissionsAndInitialize();
	}

	/** Initializes the Activity only if the permission has been granted. */
	private void checkPermissionsAndInitialize() {
		// Boilerplate for checking runtime permissions in Android.
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
			// Android 11+: legacy READ/WRITE_EXTERNAL_STORAGE are ineffective at this
			// targetSdk, so the game data folder needs "All files access"
			// (MANAGE_EXTERNAL_STORAGE).  That can only be granted from a system
			// settings screen, not a runtime dialog, so send the user there.
			if (Environment.isExternalStorageManager()) {
				permissionsGranted = true;
			} else {
				try {
					startActivityForResult(
							new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
									Uri.parse("package:" + getPackageName())),
							MANAGE_EXTERNAL_STORAGE_PERMISSION_ID);
				} catch (Exception e) {
					// Some devices don't expose the per-app screen; fall back to the list.
					startActivityForResult(
							new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
							MANAGE_EXTERNAL_STORAGE_PERMISSION_ID);
				}
			}
		} else if (ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE)
				!= PackageManager.PERMISSION_GRANTED){
			ActivityCompat.requestPermissions(
					GLES3JNIActivity.this,
					new String[] {Manifest.permission.WRITE_EXTERNAL_STORAGE},
					WRITE_EXTERNAL_STORAGE_PERMISSION_ID);
		} else if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE)
				!= PackageManager.PERMISSION_GRANTED){
			ActivityCompat.requestPermissions(
					GLES3JNIActivity.this,
					new String[] {Manifest.permission.READ_EXTERNAL_STORAGE},
					READ_EXTERNAL_STORAGE_PERMISSION_ID);
		}
		else
		{
			permissionsGranted = true;
		}

		if (permissionsGranted) {
			// Permissions have already been granted.
			create();
		}
	}

	/** Handles the user accepting the permission. */
	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
		if (requestCode == READ_EXTERNAL_STORAGE_PERMISSION_ID) {
			if (results.length > 0 && results[0] != PackageManager.PERMISSION_GRANTED) {
				System.exit(0);
			}
		}

		if (requestCode == WRITE_EXTERNAL_STORAGE_PERMISSION_ID) {
			if (results.length > 0 && results[0] != PackageManager.PERMISSION_GRANTED) {
				System.exit(0);
			}
		}

		checkPermissionsAndInitialize();
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		super.onActivityResult(requestCode, resultCode, data);
		if (requestCode == MANAGE_EXTERNAL_STORAGE_PERMISSION_ID) {
			// Returned from the All-files-access settings screen.  Bail if it still
			// isn't granted, otherwise we'd just bounce the user straight back there.
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()) {
				System.exit(0);
			} else {
				checkPermissionsAndInitialize();
			}
		}
	}

	public void create() {
		if (mNativeHandle != 0) {
			return;
		}

		getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

		//Make the directories
		new File("/sdcard/RTCWQuest/Main").mkdirs();

		//Copy the command line params file
		copy_asset("/sdcard/RTCWQuest", "commandline.txt", false);

		//and the demo version - if required
		copy_asset("/sdcard/RTCWQuest/Main", "pak0.pk3", false);

		//and the vr weapons
		copy_asset("/sdcard/RTCWQuest/Main", "z_zvr_weapons.pk3", true);

		//and some additional vr assets
		copy_asset("/sdcard/RTCWQuest/Main", "z_vr_assets.pk3", true);
		// (delete asset pak from previous version)
		File oldAssetPak = new File("/sdcard/RTCWQuest/Main/z_rtcwquest_vrmenu.pk3");
		if (oldAssetPak.exists()) {
			try {
				oldAssetPak.delete();
			} catch (Exception e) {
				e.printStackTrace();
			}
		}
		// (weapon adjustment was moved to pak, users can still use weapons_user.cfg to re-adjust)
		File oldWeaponAdjustment = new File("/sdcard/RTCWQuest/Main/weapons_vr.cfg");
		if (oldWeaponAdjustment.exists()) {
			try {
				oldWeaponAdjustment.delete();
			} catch (Exception e) {
				e.printStackTrace();
			}
		}

		//and the venom scripting improvements pak (thank-you _HELLBARON_ !!)
		copy_asset("/sdcard/RTCWQuest/Main", "sp_vpak8.pk3", false);

		//Read these from a file and pass through
		commandLineParams = new String("rtcw");

		//See if user is trying to use command line params
		if (new File("/sdcard/RTCWQuest/commandline.txt").exists()) // should exist!
		{
			BufferedReader br;
			try {
				br = new BufferedReader(new FileReader("/sdcard/RTCWQuest/commandline.txt"));
				String s;
				StringBuilder sb = new StringBuilder(0);
				while ((s = br.readLine()) != null)
					sb.append(s + " ");
				br.close();

				commandLineParams = new String(sb.toString());
			} catch (FileNotFoundException e) {
				// TODO Auto-generated catch block
				e.printStackTrace();
			} catch (IOException e) {
				// TODO Auto-generated catch block
				e.printStackTrace();
			}
		}

		try {
			setenv("RTCW_GAMELIBDIR", getApplicationInfo().nativeLibraryDir, true);
		}
		catch (Exception e)
		{

		}

		externalHapticsServiceClient = new HapticServiceClient(this, (state, desc) -> {
			Log.v(APPLICATION, "ExternalHapticsService is:" + desc);
		});

		externalHapticsServiceClient.bindService();

		mNativeHandle = GLES3JNILib.onCreate( this, commandLineParams );
		if (mSurfaceHolder != null && mSurfaceHolder.getSurface() != null && mSurfaceHolder.getSurface().isValid()) {
			GLES3JNILib.onSurfaceCreated(mNativeHandle, mSurfaceHolder.getSurface());
		}
	}

	// An OpenXR bring-up "test" wake lock used to be acquired in onResume and only released in
	// onDestroy. It kept the screen awake while the app was paused and blocked normal idle standby,
	// which made the sleep/resume behaviour untestable. It is no longer acquired anywhere; this
	// only releases one in case an older build left it held.
	private void releaseTestWakeLock() {
		if (mWakeLock != null && mWakeLock.isHeld()) {
			mWakeLock.release();
			life("released a stale wake lock");
		}
	}

	public void copy_asset(String path, String name, boolean force) {
		File f = new File(path + "/" + name);
		if (!f.exists() || force) {
			
			//Ensure we have an appropriate folder
			String fullname = path + "/" + name;
			String directory = fullname.substring(0, fullname.lastIndexOf("/"));
			new File(directory).mkdirs();
			_copy_asset(name, path + "/" + name);
		}
	}

	public void _copy_asset(String name_in, String name_out) {
		AssetManager assets = this.getAssets();

		try {
			InputStream in = assets.open(name_in);
			OutputStream out = new FileOutputStream(name_out);

			copy_stream(in, out);

			out.close();
			in.close();

		} catch (Exception e) {

			e.printStackTrace();
		}

	}

	public static void copy_stream(InputStream in, OutputStream out)
			throws IOException {
		byte[] buf = new byte[1024];
		while (true) {
			int count = in.read(buf);
			if (count <= 0)
				break;
			out.write(buf, 0, count);
		}
	}

	@Override protected void onStart()
	{
		Log.v( TAG, "GLES3JNIActivity::onStart()" );
		life( "onStart handle=" + mNativeHandle );
		super.onStart();

		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onStart(mNativeHandle, this);
		}
	}

	@Override protected void onResume()
	{
		Log.v( TAG, "GLES3JNIActivity::onResume()" );
		life( "onResume handle=" + mNativeHandle );
		super.onResume();

		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onResume(mNativeHandle);
		}
	}

	@Override protected void onPause()
	{
		Log.v( TAG, "GLES3JNIActivity::onPause()" );
		life( "onPause handle=" + mNativeHandle + " finishing=" + isFinishing() );
		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onPause(mNativeHandle);
		}
		super.onPause();
	}

	@Override protected void onStop()
	{
		Log.v( TAG, "GLES3JNIActivity::onStop()" );
		life( "onStop handle=" + mNativeHandle );
		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onStop(mNativeHandle);
		}
		super.onStop();
	}

	@Override protected void onDestroy()
	{
		Log.v( TAG, "GLES3JNIActivity::onDestroy()" );
		life( "onDestroy handle=" + mNativeHandle + " finishing=" + isFinishing() + " changingConfig=" + isChangingConfigurations() );
		releaseTestWakeLock();

		if ( mSurfaceHolder != null )
		{
			GLES3JNILib.onSurfaceDestroyed( mNativeHandle );
		}

		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onDestroy(mNativeHandle);
		}

		externalHapticsServiceClient.stopBinding();

		super.onDestroy();
		// Reset everything in case the user re opens the app
		GLES3JNIActivity.initialize();
		mNativeHandle = 0;
	}

	@Override public void surfaceCreated( SurfaceHolder holder )
	{
		Log.v( TAG, "GLES3JNIActivity::surfaceCreated()" );
		life( "surfaceCreated surface=" + System.identityHashCode( holder.getSurface() ) + " valid=" + holder.getSurface().isValid() );
		mSurfaceHolder = holder;
		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onSurfaceCreated( mNativeHandle, holder.getSurface() );
		}
	}

	@Override public void surfaceChanged( SurfaceHolder holder, int format, int width, int height )
	{
		Log.v( TAG, "GLES3JNIActivity::surfaceChanged()" );
		life( "surfaceChanged surface=" + System.identityHashCode( holder.getSurface() ) + " " + width + "x" + height + " format=" + format );
		mSurfaceHolder = holder;
		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onSurfaceChanged( mNativeHandle, holder.getSurface() );
		}
	}
	
	@Override public void surfaceDestroyed( SurfaceHolder holder )
	{
		Log.v( TAG, "GLES3JNIActivity::surfaceDestroyed()" );
		life( "surfaceDestroyed handle=" + mNativeHandle );
		if ( mNativeHandle != 0 )
		{
			GLES3JNILib.onSurfaceDestroyed( mNativeHandle );
			mSurfaceHolder = null;
		}
	}
}
