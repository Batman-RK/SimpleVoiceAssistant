package com.example.simplevoiceassistant;

import android.service.voice.VoiceInteractionService;
import android.util.Log;

public class MyVoiceInteractionService extends VoiceInteractionService {
    private static final String TAG = "SimpleAssistant";

    @Override
    public void onReady() {
        super.onReady();
        Log.i(TAG, "VoiceInteractionService is READY! (onReady called)");
    }
    
    @Override
    public void onShutdown() {
        super.onShutdown();
        Log.i(TAG, "VoiceInteractionService SHUTDOWN");
    }
}