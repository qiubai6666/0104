#ifndef ELIDEDLABEL_H
#define ELIDEDLABEL_H

#include <QLabel>
#include <QPainter>

// QLabel that clips long device names with an ellipsis while keeping the full
// text for accessibility and tooltips.
class ElidedLabel : public QLabel
{
public:
    explicit ElidedLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        painter.setFont(font());
        const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight,
                                                       contentsRect().width());
        painter.drawText(contentsRect(), alignment(), shown);
    }
};

#endif // ELIDEDLABEL_H
