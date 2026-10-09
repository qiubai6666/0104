#ifndef MAINMENUBUTTON_H
#define MAINMENUBUTTON_H

#include <QPushButton>
#include <QColor>
#include <QVariantAnimation>

// Icon-only capsule actions; mouse hover and keyboard focus stay independent.
class MainMenuButton : public QPushButton
{
public:
    explicit MainMenuButton(QWidget *parent = nullptr);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void updateBackground(bool animate = true);
    QVariantAnimation backgroundAnimation;
    QColor backgroundColor = QColor(255, 255, 255, 0);
    bool hovered = false;
    bool keyboardFocus = false;
};

#endif // MAINMENUBUTTON_H
