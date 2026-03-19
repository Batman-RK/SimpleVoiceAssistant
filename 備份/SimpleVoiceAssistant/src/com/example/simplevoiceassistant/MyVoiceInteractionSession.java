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

    // --- 錄音檔案儲存 ---
    private java.io.File mAudioFile;
    private java.io.FileOutputStream mFileOutputStream;
    private long mTotalAudioLen = 0;

    // --- UI 元件 ---
    private TextView mStatusTextView;
    private Handler mMainHandler = new Handler(Looper.getMainLooper());

    private static final int NOISE_THRESHOLD = 500; // 門檻值，可依背景噪音微調

    public MyVoiceInteractionSession(Context context) {
        super(context);
        Log.d(TAG, "MyVoiceInteractionSession: 實例已建立");
    }

    @Override
    public View onCreateContentView() {
        FrameLayout container = new FrameLayout(getContext());
        
        mStatusTextView = new TextView(getContext());
        mStatusTextView.setText("🎙️ RTK Assistant Listening...");
        mStatusTextView.setTextSize(24);
        mStatusTextView.setTextColor(Color.WHITE);
        mStatusTextView.setBackgroundColor(Color.parseColor("#CC000000")); // 深色半透明背景
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
            // 初始化檔案儲存
            java.io.File dir = new java.io.File("/storage/emulated/0/Recordings");
            if (!dir.exists()) {
                dir.mkdirs();
            }
            mAudioFile = new java.io.File(dir, "voice_test_" + System.currentTimeMillis() + ".wav");
            mFileOutputStream = new java.io.FileOutputStream(mAudioFile);
            
            // 先寫入 44 字节的空白作為 Header 佔位
            byte[] headerBlank = new byte[44];
            mFileOutputStream.write(headerBlank);
            mTotalAudioLen = 0;

            int minBufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL, ENCODING);
            mAudioRecord = new AudioRecord(MediaRecorder.AudioSource.MIC, 
                    SAMPLE_RATE, CHANNEL, ENCODING, minBufferSize);

            if (mAudioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                Log.e(TAG, "AudioRecord 初始化失敗！");
                updateUiText("❌ 麥克風初始化失敗");
                return;
            }

            mAudioRecord.startRecording();
            mIsRecording = true;
            updateUiText("🎙️ 正在傾聽中...");

            mRecordThread = new Thread(this::recordLoop);
            mRecordThread.start();

        } catch (Exception e) {
            Log.e(TAG, "啟動錄製或建立檔案錯誤: ", e);
            updateUiText("❌ 啟動失敗: " + e.getMessage());
        }
    }

    private void recordLoop() {
        int bufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL, ENCODING);
        short[] buffer = new short[bufferSize / 2]; // 16-bit 需讀取為 short

        while (mIsRecording) {
            if (mAudioRecord == null) break;
            
            int readBytes = mAudioRecord.read(buffer, 0, buffer.length);
            if (readBytes > 0) {
                // 新增：寫入檔案
                try {
                    byte[] byteBuf = shortToByte(buffer, readBytes);
                    if (mFileOutputStream != null) {
                        mFileOutputStream.write(byteBuf);
                        mTotalAudioLen += byteBuf.length;
                    }
                } catch (java.io.IOException e) {
                    Log.e(TAG, "檔案寫入錯誤: ", e);
                }

                // 計算均方根 (RMS) 振幅
                long sum = 0;
                for (int i = 0; i < readBytes; i++) {
                    sum += buffer[i] * buffer[i];
                }
                double rms = Math.sqrt(sum / readBytes);

                if (rms > NOISE_THRESHOLD) {
                    mMainHandler.post(() -> mStatusTextView.setText("🎙️ 正在接收聲音... (dB)"));
                } else {
                    mMainHandler.post(() -> mStatusTextView.setText("🎙️ 正在傾聽中..."));
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

        // 新增：處理與更新 WAV 檔案頭
        if (mFileOutputStream != null) {
            try {
                mFileOutputStream.close();
                mFileOutputStream = null;
                updateWavHeader(mAudioFile, mTotalAudioLen);
                updateUiText("🎙️ 錄音檔存檔完成！");
                Log.d(TAG, "錄音檔保存至: " + mAudioFile.getAbsolutePath());
            } catch (java.io.IOException e) {
                Log.e(TAG, "關閉檔案錯誤: ", e);
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

    // --- 新增：將 short 陣列轉 byte 陣列 ---
    private byte[] shortToByte(short[] sData, int size) {
        byte[] bytes = new byte[size * 2];
        for (int i = 0; i < size; i++) {
            bytes[i * 2] = (byte) (sData[i] & 0x00FF);
            bytes[(i * 2) + 1] = (byte) (sData[i] >> 8);
        }
        return bytes;
    }

    // --- 新增：回寫 WAV 標頭檔 ---
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
            Log.e(TAG, "更新 WAV 標頭錯誤: ", e);
        }
    }
}