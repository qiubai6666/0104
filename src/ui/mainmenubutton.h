#ifndef MAINMENUBUTTON_H
#define MAINMENUBUTTON_H

#include <QPushButton>
#include <QColor>
#include <QTimer>
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
    void updateIconColor(bool animate = true);
    QVariantAnimation colorAnimation;
    QTimer clickFeedbackTimer;
    QColor iconColor;
    bool hovered = false;
    bool keyboardFocus = false;
};

#endif // MAINMENUBUTTON_H
