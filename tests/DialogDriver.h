#pragma once
#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>
#include <QtTest>
#include <functional>

// Poll outside the worker's completion callback: a modal dialog starts its own
// event loop.
inline void whenVisible(QWidget *owner, const QString &name, std::function<void(QWidget *)> action)
{
    auto *timer = new QTimer(owner);
    QElapsedTimer elapsed;
    elapsed.start();
    QObject::connect(timer, &QTimer::timeout, owner,
                     [owner, timer, name, action, elapsed]
                     {
                         for (auto *widget : owner->findChildren<QWidget *>(name))
                             if (widget->isVisible())
                             {
                                 timer->stop();
                                 timer->deleteLater();
                                 action(widget);
                                 return;
                             }
                         if (elapsed.elapsed() > 10000)
                         {
                             timer->stop();
                             timer->deleteLater();
                             QTest::qFail(qPrintable("Timed out waiting for " + name), __FILE__,
                                          __LINE__);
                             for (auto *dialog : owner->findChildren<QDialog *>())
                                 if (dialog->isVisible())
                                     dialog->reject();
                         }
                     });
    timer->start(10);
}
