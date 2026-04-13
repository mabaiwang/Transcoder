#include <QApplication>
#include <QIcon>
#include <QStyleFactory>

#include "TranscoderWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));

    TranscoderWindow window;
    window.showMaximized();
    return app.exec();
}
