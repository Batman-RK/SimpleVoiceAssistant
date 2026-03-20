package com.example.simplevoiceassistant;

import android.content.Context;
import android.graphics.Color;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.service.voice.VoiceInteractionSession;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

public class MyVoiceInteractionSession extends VoiceInteractionSession {
    private static final String TAG = "SimpleAssistantSession";

    // --- AudioRecord 設定 ---
    private static final int SAMPLE_RATE = 16000;
    private static final int CHANNEL = AudioFormat.CHANNEL_IN_MONO;
    private static final int ENCODING = AudioFormat.ENCODING_PCM_16BIT;

    private AudioRecord mAudioRecord;
    private boolean mIsRecording = false;
    private Thread mRecordThread;

    // --- Audio file saved ---
    private java.io.File mAudioFile;
    private java.io.FileOutputStream mFileOutputStream;
    private long mTotalAudioLen = 0;

    // --- Audio Effects ---
    private android.media.audiofx.AcousticEchoCanceler mAec;

    // --- UI components ---
    private TextView mStatusTextView;
    private Handler mMainHandler = new Handler(Looper.getMainLooper());

    private static final int NOISE_THRESHOLD = 500; // Threshold value, can be adjusted according to background noise

    public MyVoiceInteractionSession(Context context) {
        super(context);
        Log.d(TAG, "MyVoiceInteractionSession: Instance created");
    }

    @Override
    public View onCreateContentView() {
        FrameLayout container = new FrameLayout(getContext());
        
        mStatusTextView = new TextView(getContext());
        mStatusTextView.setText("🎙️ RTK Assistant Listening...");
        mStatusTextView.setTextSize(24);
        mStatusTextView.setTextColor(Color.WHITE);
        mStatusTextView.setBackgroundColor(Color.parseColor("#CC000000")); // Dark semi-transparent background
        mStatusTextView.setPadding(40, 40, 40, 40);
        mStatusTextView.setGravity(Gravity.CENTER);
        
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                200,
                Gravity.BOTTOM);
        container.addView(mStatusTextView, lp);
        
        return container;
    }

    @Override
    public void onShow(Bundle args, int showFlags) {
        super.onShow(args, showFlags);
        Log.d(TAG, "onShow: Session is VISIBLE. Starting recording...");
        startRecording();
    }

    @Override
    public void onHide() {
        super.onHide();
        Log.d(TAG, "onHide: Session HIDDEN. Stopping recording...");
        stopRecording();
    }

    private void startRecording() {
        if (mIsRecording) return;

        try {
            // Initialize file storage
            java.io.File dir = new java.io.File("/storage/emulated/0/Recordings");
            if (!dir.exists()) {
                dir.mkdirs();
            }
            mAudioFile = new java.io.File(dir, "voice_test_" + System.currentTimeMillis() + ".wav");
            mFileOutputStream = new java.io.FileOutputStream(mAudioFile);
            
            // Write 44 bytes of blank space as Header placeholder
            byte[] headerBlank = new byte[44];
            mFileOutputStream.write(headerBlank);
            mTotalAudioLen = 0;

            int minBufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL, ENCODING);
            mAudioRecord = new AudioRecord(MediaRecorder.AudioSource.MIC, 
                    SAMPLE_RATE, CHANNEL, ENCODING, minBufferSize);

            if (mAudioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                Log.e(TAG, "AudioRecord initialization failed!");
                updateUiText("❌ Microphone initialization failed!");
                return;
            }

            // --- Enable AEC (Acoustic Echo Canceler) ---
            if (android.media.audiofx.AcousticEchoCanceler.isAvailable()) {
                mAec = android.media.audiofx.AcousticEchoCanceler.create(mAudioRecord.getAudioSessionId());
                if (mAec != null) {
                    mAec.setEnabled(true);
                    Log.d(TAG, "AEC successfully enabled!");
                } else {
                    Log.e(TAG, "Hardware supports AEC, but failed to create AEC instance.");
                }
            } else {
                Log.w(TAG, "Hardware doesn't support AEC.");
            }

            mAudioRecord.startRecording();
            mIsRecording = true;
            updateUiText("🎙️ [Usan test]正在傾聽中...");

            mRecordThread = new Thread(this::recordLoop);
            mRecordThread.start();

        } catch (Exception e) {
            Log.e(TAG, "Error starting recording or creating file: ", e);
            updateUiText("❌ [Usan test]啟動失敗: " + e.getMessage());
        }
    }

    private void recordLoop() {
        int bufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL, ENCODING);
        short[] buffer = new short[bufferSize / 2]; // 16-bit should be read as short

        while (mIsRecording) {
            if (mAudioRecord == null) break;
            
            int readBytes = mAudioRecord.read(buffer, 0, buffer.length);
            if (readBytes > 0) {
                // Add: Write to file
                try {
                    byte[] byteBuf = shortToByte(buffer, readBytes);
                    if (mFileOutputStream != null) {
                        mFileOutputStream.write(byteBuf);
                        mTotalAudioLen += byteBuf.length;
                    }
                } catch (java.io.IOException e) {
                    Log.e(TAG, "File write error: ", e);
                }

                // Calculate Root Mean Square (RMS) amplitude
                long sum = 0;
                for (int i = 0; i < readBytes; i++) {
                    sum += buffer[i] * buffer[i];
                }
                double rms = Math.sqrt(sum / readBytes);

                if (rms > NOISE_THRESHOLD) {
                    mMainHandler.post(() -> mStatusTextView.setText("🎙️ [Usan test]正在接收聲音... (dB)"));
                } else {
                    mMainHandler.post(() -> mStatusTextView.setText("🎙️ [Usan test]正在傾聽中..."));
                }
            }
        }
    }

    private void stopRecording() {
        mIsRecording = false;
        if (mRecordThread != null) {
            try {
                mRecordThread.join(500); 
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
            mRecordThread = null;
        }

        if (mAudioRecord != null) {
            try {
                if (mAudioRecord.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING) {
                    mAudioRecord.stop();
                }
            } catch (IllegalStateException e) {
                e.printStackTrace();
            }
            mAudioRecord.release();
            mAudioRecord = null;
        }

        // --- Release AEC ---
        if (mAec != null) {
            mAec.release();
            mAec = null;
            Log.d(TAG, "AEC released");
        }

        // Add: Process and update WAV file header
        if (mFileOutputStream != null) {
            try {
                mFileOutputStream.close();
                mFileOutputStream = null;
                updateWavHeader(mAudioFile, mTotalAudioLen);
                updateUiText("🎙️ [UsanTest]錄音檔存檔完成！");
                Log.d(TAG, "Audio file saved to: " + mAudioFile.getAbsolutePath());
            } catch (java.io.IOException e) {
                Log.e(TAG, "Error closing file: ", e);
            }
        }
    }

    private void updateUiText(final String text) {
        mMainHandler.post(() -> {
            if (mStatusTextView != null) {
                mStatusTextView.setText(text);
            }
        });
    }

    // --- Add: Convert short array to byte array ---
    private byte[] shortToByte(short[] sData, int size) {
        byte[] bytes = new byte[size * 2];
        for (int i = 0; i < size; i++) {
            bytes[i * 2] = (byte) (sData[i] & 0x00FF);
            bytes[(i * 2) + 1] = (byte) (sData[i] >> 8);
        }
        return bytes;
    }

    // --- Add: Write WAV file header ---
    private void updateWavHeader(java.io.File file, long totalAudioLen) {
        try {
            java.io.RandomAccessFile raf = new java.io.RandomAccessFile(file, "rw");
            long totalDataLen = totalAudioLen + 36;
            long longSampleRate = SAMPLE_RATE;
            int channels = 1;
            long byteRate = 16 * SAMPLE_RATE * channels / 8;

            byte[] header = new byte[44];
            header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
            header[4] = (byte) (totalDataLen & 0xff);
            header[5] = (byte) ((totalDataLen >> 8) & 0xff);
            header[6] = (byte) ((totalDataLen >> 16) & 0xff);
            header[7] = (byte) ((totalDataLen >> 24) & 0xff);
            header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
            header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
            header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
            header[20] = 1; header[21] = 0;
            header[22] = (byte) channels; header[23] = 0;
            header[24] = (byte) (longSampleRate & 0xff);
            header[25] = (byte) ((longSampleRate >> 8) & 0xff);
            header[26] = (byte) ((longSampleRate >> 16) & 0xff);
            header[27] = (byte) ((longSampleRate >> 24) & 0xff);
            header[28] = (byte) (byteRate & 0xff);
            header[29] = (byte) ((byteRate >> 8) & 0xff);
            header[30] = (byte) ((byteRate >> 16) & 0xff);
            header[31] = (byte) ((byteRate >> 24) & 0xff);
            header[32] = (byte) (2); header[33] = 0;
            header[34] = 16; header[35] = 0;
            header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
            header[40] = (byte) (totalAudioLen & 0xff);
            header[41] = (byte) ((totalAudioLen >> 8) & 0xff);
            header[42] = (byte) ((totalAudioLen >> 16) & 0xff);
            header[43] = (byte) ((totalAudioLen >> 24) & 0xff);

            raf.seek(0);
            raf.write(header);
            raf.close();
        } catch (Exception e) {
            Log.e(TAG, "Update WAV header error: ", e);
        }
    }
}