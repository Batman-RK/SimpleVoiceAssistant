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
            int minBufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL, ENCODING);
            // 先使用確實在 APK 跑得通的 MediaRecorder.AudioSource.MIC
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

        } catch (SecurityException e) {
            Log.e(TAG, "錄音安全權限錯誤: ", e);
            updateUiText("❌ 缺少錄音權限");
        }
    }

    private void recordLoop() {
        int bufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL, ENCODING);
        short[] buffer = new short[bufferSize / 2]; // 16-bit 需讀取為 short

        while (mIsRecording) {
            if (mAudioRecord == null) break;
            
            int readBytes = mAudioRecord.read(buffer, 0, buffer.length);
            if (readBytes > 0) {
                // 計算均方根 (RMS) 振幅
                long sum = 0;
                for (int i = 0; i < readBytes; i++) {
                    sum += buffer[i] * buffer[i];
                }
                double rms = Math.sqrt(sum / readBytes);

                // 當分貝大於門檻值，模擬收到聲音狀態
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
                mRecordThread.join(500); // 等待線程安全結束
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
    }

    private void updateUiText(final String text) {
        mMainHandler.post(() -> {
            if (mStatusTextView != null) {
                mStatusTextView.setText(text);
            }
        });
    }
}