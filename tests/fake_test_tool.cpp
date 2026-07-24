#include <QCoreApplication>
#include <QTextStream>
#include <QThread>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    int exitCode = 0;
    int sleepMs = 0;
    QString standardOutput;
    QString standardError;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--exit") && index + 1 < arguments.size()) {
            exitCode = arguments.at(++index).toInt();
        } else if (argument == QStringLiteral("--sleep-ms")
                   && index + 1 < arguments.size()) {
            sleepMs = arguments.at(++index).toInt();
        } else if (argument == QStringLiteral("--stdout")
                   && index + 1 < arguments.size()) {
            standardOutput = arguments.at(++index);
        } else if (argument == QStringLiteral("--stderr")
                   && index + 1 < arguments.size()) {
            standardError = arguments.at(++index);
        }
    }
    if (!standardOutput.isEmpty()) {
        QTextStream(stdout) << standardOutput << Qt::endl;
    }
    if (!standardError.isEmpty()) {
        QTextStream(stderr) << standardError << Qt::endl;
    }
    if (sleepMs > 0) {
        QThread::msleep(static_cast<unsigned long>(sleepMs));
    }
    return exitCode;
}
