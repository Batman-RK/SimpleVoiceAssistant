package com.example.simplevoiceassistant;

import android.os.Bundle;
import android.service.voice.VoiceInteractionSession;
import android.service.voice.VoiceInteractionSessionService;
import android.util.Log;

public class MyVoiceInteractionSessionService extends VoiceInteractionSessionService {
    private static final String TAG = "SimpleAssistantSessionService";

    @Override
    public VoiceInteractionSession onNewSession(Bundle args) {
        Log.d(TAG, "onNewSession: 系統正在請求建立新的語音會話");
        return new MyVoiceInteractionSession(this);
    }
}