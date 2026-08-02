#pragma once

namespace MqttConfig {

inline constexpr char BROKER_URI[] = "mqtt://182.208.84.190:1883";
inline constexpr char CLIENT_ID_FORMAT[] = "dy-ep4-%s";
inline constexpr int EQUIPMENT_CODE_LENGTH = 12;
inline constexpr char TELEMETRY_TOPIC_FORMAT[] = "dryers/%s/telemetry";
inline constexpr char EVENT_TOPIC_FORMAT[] = "dryers/%s/event";
inline constexpr char COMMAND_TOPIC_FORMAT[] = "dryers/%s/command";

inline constexpr int QOS = 1;
inline constexpr int KEEPALIVE_SECONDS = 30;
inline constexpr int RECONNECT_TIMEOUT_MS = 5000;
inline constexpr int TIME_TEXT_SIZE = 20;  // YYYY-MM-DD HH:MM:SS + NUL
inline constexpr int TELEMETRY_FRAME_SIZE = 172;

}  // namespace MqttConfig
