package com.example.simplevoiceassistant;

import android.content.Intent;
import android.speech.RecognitionService;
import android.os.Bundle;
import android.util.Log;

public class MyRecognitionService extends RecognitionService {
    private static final String TAG = "MyRecognitionService";

    @Override
    protected void onStartListening(Intent recognizerIntent, Callback listener) {
        Log.d(TAG, "onStartListening: The system requests to start audio recording and recognition");
        // The logic for AudioRecord will be implemented here in the future.
    }

    @Override
    protected void onCancel(Callback listener) {
        Log.d(TAG, "onCancel: Recognition canceled");
    }

    @Override
    protected void onStopListening(Callback listener) {
        Log.d(TAG, "onStopListening: Stop listening");
    }
}