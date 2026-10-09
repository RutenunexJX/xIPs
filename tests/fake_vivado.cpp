#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <cstdio>
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto mode = qEnvironmentVariable("FTB_FAKE_MODE");
    if (mode == "hang") {
        QFile pid(QFileInfo(qEnvironmentVariable("FTB_SUCCESS_FILE")).dir().filePath("fake.pid"));
        if (!pid.open(QIODevice::WriteOnly)) return 17;
        pid.write(QByteArray::number(QCoreApplication::applicationPid())); pid.close();
        for (;;) QThread::msleep(100);
    }
    const auto root = qEnvironmentVariable("FTB_PROJECT_ROOT");
    const auto xpr = qEnvironmentVariable("FTB_XPR");
    if (root.isEmpty() || !xpr.startsWith(root + '/')) return 10;
    QFile project(xpr);
    if (!project.open(QIODevice::ReadOnly)) return 11;
    const auto bytes = project.readAll(); project.close();
    const auto original = qEnvironmentVariable("FTB_TEST_SOURCE").toUtf8();
    if (!original.isEmpty() && bytes.contains(original)) return 12;
    QFile script("reset.tcl");
    if (!script.open(QIODevice::ReadOnly) || !script.readAll().contains("\n    reset_project\n")) return 13;
    std::puts("FAKE: copy opened, reset requested"); std::fflush(stdout);
    if (mode == "fail") return 3;
    const auto name = QFileInfo(xpr).completeBaseName();
    if (!QDir(root + '/' + name + ".runs").removeRecursively()) return 14;
    // Model deletion of generated products as well as run results during reset.
    for (const auto* suffix : {".gen", ".ip_user_files"}) {
        QDir generated(root + '/' + name + suffix);
        if (generated.exists() && !generated.removeRecursively()) return 18;
    }
    QFile extra(root + "/generated.log"); if (!extra.open(QIODevice::WriteOnly)) return 15;
    extra.write("generated"); extra.close();
    QFile marker(qEnvironmentVariable("FTB_SUCCESS_FILE"));
    if (mode != "missing-marker") { if (!marker.open(QIODevice::WriteOnly)) return 16; marker.write("reset_project completed\n"); }
    return 0;
}
