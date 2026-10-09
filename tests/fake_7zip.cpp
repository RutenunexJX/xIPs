#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const auto mode = qEnvironmentVariable("FTB_FAKE_7ZIP");
    const int marker = args.indexOf("--");
    if (marker < 0 || marker + 1 >= args.size()) return 10;
    const auto archive = args[marker + 1];
    if (mode == "hang") {
        QFile pid(QFileInfo(archive).dir().filePath("fake-7zip.pid"));
        if (!pid.open(QIODevice::WriteOnly)) return 11;
        pid.write(QByteArray::number(app.applicationPid())); pid.close();
        for (;;) QThread::msleep(100);
    }
    if (args[1] == "a" && (mode == "invalid" || mode == "test-failure")) {
        QFile output(archive);
        if (!output.open(QIODevice::WriteOnly)) return 12;
        output.write(mode == "invalid" ? QByteArray("invalid package") : QByteArray::fromHex("377abcaf271c"));
        return 0;
    }
    return 2;
}
