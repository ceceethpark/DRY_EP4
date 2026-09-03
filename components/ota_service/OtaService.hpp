#pragma once

class OtaService {
public:
    bool start();

private:
    static void discoveryTask(void *context);
    static void restartTask(void *context);
    static int infoHandler(void *request);
    static int updateHandler(void *request);

    void *_httpServer = nullptr;
    bool _started = false;
};
