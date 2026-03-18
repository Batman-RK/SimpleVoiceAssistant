package com.example.simplevoiceassistant;

import android.content.Intent;
import android.speech.RecognitionService;
import android.os.Bundle;
import android.util.Log;

public class MyRecognitionService extends RecognitionService {
    private static final String TAG = "MyRecognitionService";

    @Override
    protected void onStartListening(Intent recognizerIntent, Callback listener) {
        Log.d(TAG, "onStartListening: 系統請求開始錄音辨識");
        // 這裡未來會實作 AudioRecord 邏輯
    }

    @Override
    protected void onCancel(Callback listener) {
        Log.d(TAG, "onCancel: 辨識取消");
    }

    @Override
    protected void onStopListening(Callback listener) {
        Log.d(TAG, "onStopListening: 停止錄音");
    }
}