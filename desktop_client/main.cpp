#include <QApplication>
#include <QByteArray>
#include <QBrush>
#include <QCheckBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTcpSocket>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QString sourceName(const QString& source) {
    return source == QStringLiteral("voice") ? QStringLiteral("语音")
                                              : QStringLiteral("文字");
}

QString modeName(const QString& mode) {
    if (mode == QStringLiteral("voice")) return QStringLiteral("语音输入");
    if (mode == QStringLiteral("text")) return QStringLiteral("文字输入");
    return QStringLiteral("未知");
}

QString boardName(const QString& status) {
    if (status == QStringLiteral("connected")) return QStringLiteral("已连接");
    if (status == QStringLiteral("disconnected")) return QStringLiteral("未连接");
    return QStringLiteral("未知");
}

}  // namespace

class DesktopClientWindow final : public QMainWindow {
public:
    DesktopClientWindow() {
        setWindowTitle(QStringLiteral("智能语音助手上位机"));
        resize(1280, 800);
        setMinimumSize(1024, 680);
        buildUi();
        wireSocket();
        appendLog(QStringLiteral("信息"), QStringLiteral("界面"),
                  QStringLiteral("控制台已启动，等待连接服务。"));
    }

private:
    void buildUi() {
        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->setSpacing(12);

        auto* title_layout = new QHBoxLayout();
        auto* title = new QLabel(QStringLiteral("智能语音助手控制台"), central);
        title->setObjectName(QStringLiteral("windowTitle"));
        auto* subtitle = new QLabel(QStringLiteral("语音、文字与板端运行状态"), central);
        subtitle->setObjectName(QStringLiteral("windowSubtitle"));
        title_layout->addWidget(title);
        title_layout->addWidget(subtitle);
        title_layout->addStretch();
        layout->addLayout(title_layout);

        auto* connection_box = new QGroupBox(QStringLiteral("控制服务连接"), central);
        auto* connection_layout = new QHBoxLayout(connection_box);
        host_edit_ = new QLineEdit(QStringLiteral("127.0.0.1"), connection_box);
        host_edit_->setClearButtonEnabled(true);
        port_spin_ = new QSpinBox(connection_box);
        port_spin_->setRange(1, 65535);
        port_spin_->setValue(8081);
        connect_button_ = new QPushButton(QStringLiteral("连接服务"), connection_box);
        reset_button_ = new QPushButton(QStringLiteral("新建会话"), connection_box);
        reset_button_->setEnabled(false);
        connection_layout->addWidget(new QLabel(QStringLiteral("地址"), connection_box));
        connection_layout->addWidget(host_edit_, 1);
        connection_layout->addWidget(new QLabel(QStringLiteral("控制端口"), connection_box));
        connection_layout->addWidget(port_spin_);
        connection_layout->addWidget(connect_button_);
        connection_layout->addWidget(reset_button_);
        layout->addWidget(connection_box);

        auto* overview_box = new QGroupBox(QStringLiteral("系统状态"), central);
        auto* overview_layout = new QGridLayout(overview_box);
        overview_layout->setHorizontalSpacing(10);
        overview_layout->setVerticalSpacing(10);
        overview_layout->addWidget(createStatusCard(QStringLiteral("服务连接"), service_value_), 0, 0);
        overview_layout->addWidget(createStatusCard(QStringLiteral("开发板"), board_value_), 0, 1);
        overview_layout->addWidget(createStatusCard(QStringLiteral("输入模式"), mode_value_), 0, 2);
        overview_layout->addWidget(createStatusCard(QStringLiteral("最近意图"), intent_value_), 0, 3);
        overview_layout->addWidget(createStatusCard(QStringLiteral("本轮输出"), output_value_), 0, 4);
        layout->addWidget(overview_box);

        auto* metric_box = new QGroupBox(QStringLiteral("本次会话"), central);
        auto* metric_layout = new QHBoxLayout(metric_box);
        metric_layout->addWidget(createMetric(QStringLiteral("交互轮次"), turns_value_));
        metric_layout->addWidget(createMetric(QStringLiteral("最近回复耗时"), latency_value_));
        metric_layout->addWidget(createMetric(QStringLiteral("运行事件"), event_count_value_));
        metric_layout->addStretch();
        layout->addWidget(metric_box);

        auto* splitter = new QSplitter(Qt::Horizontal, central);
        splitter->setChildrenCollapsible(false);
        splitter->addWidget(createConversationPanel());
        splitter->addWidget(createLogPanel());
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        layout->addWidget(splitter, 1);

        setCentralWidget(central);
        statusBar()->showMessage(QStringLiteral("就绪"));
        setStyleSheet(QStringLiteral(
            "QWidget { font-family: 'Microsoft YaHei UI', 'Noto Sans CJK SC'; }"
            "QGroupBox { font-weight: 600; border: 1px solid #D8E0EA; border-radius: 7px;"
            " margin-top: 10px; padding: 10px 8px 8px 8px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #243447; }"
            "QFrame#statusCard { background: #F7FAFD; border: 1px solid #DCE6F1; border-radius: 7px; }"
            "QLabel#statusTitle { color: #64748B; font-size: 12px; }"
            "QLabel#statusValue { color: #163A5F; font-size: 16px; font-weight: 600; }"
            "QLabel#metricTitle { color: #64748B; font-size: 12px; }"
            "QLabel#metricValue { color: #0F4C81; font-size: 18px; font-weight: 600; }"
            "QLabel#windowTitle { color: #123B62; font-size: 24px; font-weight: 700; }"
            "QLabel#windowSubtitle { color: #64748B; padding-left: 8px; }"
            "QPushButton { min-height: 28px; padding: 3px 14px; }"
            "QTableWidget { border: 1px solid #D8E0EA; border-radius: 5px; gridline-color: #E6ECF2; }"));
    }

    QFrame* createStatusCard(const QString& title, QLabel*& value) {
        auto* card = new QFrame();
        card->setObjectName(QStringLiteral("statusCard"));
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(12, 9, 12, 9);
        auto* title_label = new QLabel(title, card);
        title_label->setObjectName(QStringLiteral("statusTitle"));
        value = new QLabel(QStringLiteral("--"), card);
        value->setObjectName(QStringLiteral("statusValue"));
        value->setWordWrap(true);
        layout->addWidget(title_label);
        layout->addWidget(value);
        return card;
    }

    QWidget* createMetric(const QString& title, QLabel*& value) {
        auto* widget = new QWidget();
        auto* layout = new QVBoxLayout(widget);
        layout->setContentsMargins(10, 2, 28, 2);
        auto* title_label = new QLabel(title, widget);
        title_label->setObjectName(QStringLiteral("metricTitle"));
        value = new QLabel(QStringLiteral("0"), widget);
        value->setObjectName(QStringLiteral("metricValue"));
        layout->addWidget(title_label);
        layout->addWidget(value);
        return widget;
    }

    QWidget* createConversationPanel() {
        auto* panel = new QGroupBox(QStringLiteral("对话"));
        auto* layout = new QVBoxLayout(panel);
        conversation_ = new QTextEdit(panel);
        conversation_->setReadOnly(true);
        conversation_->setPlaceholderText(QStringLiteral("服务连接后，语音识别结果和文字对话会显示在这里。"));
        layout->addWidget(conversation_, 1);

        input_ = new QPlainTextEdit(panel);
        input_->setPlaceholderText(QStringLiteral("输入文字后发送；开发板按键录音时，状态卡会自动切换为语音输入。"));
        input_->setFixedHeight(78);
        layout->addWidget(input_);

        auto* actions = new QHBoxLayout();
        speak_check_ = new QCheckBox(QStringLiteral("发送后在板端朗读"), panel);
        send_button_ = new QPushButton(QStringLiteral("发送文字"), panel);
        send_button_->setDefault(true);
        actions->addWidget(speak_check_);
        actions->addStretch();
        actions->addWidget(send_button_);
        layout->addLayout(actions);
        return panel;
    }

    QWidget* createLogPanel() {
        auto* panel = new QGroupBox(QStringLiteral("运行事件"));
        auto* layout = new QVBoxLayout(panel);
        event_table_ = new QTableWidget(0, 4, panel);
        event_table_->setHorizontalHeaderLabels(
            {QStringLiteral("时间"), QStringLiteral("级别"), QStringLiteral("类别"), QStringLiteral("内容")});
        event_table_->verticalHeader()->setVisible(false);
        event_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        event_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        event_table_->setAlternatingRowColors(true);
        event_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        event_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        event_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        event_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        layout->addWidget(event_table_);
        return panel;
    }

    void wireSocket() {
        connect(connect_button_, &QPushButton::clicked, this, [this] { toggleConnection(); });
        connect(reset_button_, &QPushButton::clicked, this, [this] { resetConversation(); });
        connect(send_button_, &QPushButton::clicked, this, [this] { submitText(); });
        connect(input_, &QPlainTextEdit::textChanged, this, [this] { updateSendButton(); });
        connect(&socket_, &QTcpSocket::connected, this, [this] {
            service_value_->setText(QStringLiteral("已连接"));
            connect_button_->setText(QStringLiteral("断开服务"));
            reset_button_->setEnabled(true);
            host_edit_->setEnabled(false);
            port_spin_->setEnabled(false);
            updateSendButton();
            appendLog(QStringLiteral("信息"), QStringLiteral("服务"), QStringLiteral("控制服务已连接。"));
            sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("get_status")}});
        });
        connect(&socket_, &QTcpSocket::disconnected, this, [this] {
            service_value_->setText(QStringLiteral("未连接"));
            connect_button_->setText(QStringLiteral("连接服务"));
            reset_button_->setEnabled(false);
            host_edit_->setEnabled(true);
            port_spin_->setEnabled(true);
            reply_open_ = false;
            updateSendButton();
            appendLog(QStringLiteral("信息"), QStringLiteral("服务"), QStringLiteral("控制服务已断开。"));
        });
        connect(&socket_, &QTcpSocket::readyRead, this, [this] { readMessages(); });
        connect(&socket_, &QTcpSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) {
                    service_value_->setText(QStringLiteral("连接错误"));
                    appendLog(QStringLiteral("错误"), QStringLiteral("服务"), socket_.errorString());
                });
        updateSendButton();
    }

    void toggleConnection() {
        if (socket_.state() == QAbstractSocket::ConnectedState ||
            socket_.state() == QAbstractSocket::ConnectingState) {
            socket_.disconnectFromHost();
            return;
        }
        service_value_->setText(QStringLiteral("连接中"));
        statusBar()->showMessage(QStringLiteral("正在连接控制服务…"));
        socket_.connectToHost(host_edit_->text().trimmed(),
                              static_cast<quint16>(port_spin_->value()));
    }

    void resetConversation() {
        if (socket_.state() != QAbstractSocket::ConnectedState) return;
        const QString request_id = nextRequestId(QStringLiteral("reset"));
        sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("reset_conversation")},
                             {QStringLiteral("request_id"), request_id}});
        conversation_->append(QStringLiteral("<hr><span style=\"color:#64748b\">已请求开始新会话</span>"));
        appendLog(QStringLiteral("信息"), QStringLiteral("会话"), QStringLiteral("已请求重置会话上下文。"));
    }

    void submitText() {
        const QString text = input_->toPlainText().trimmed();
        if (text.isEmpty() || socket_.state() != QAbstractSocket::ConnectedState) return;

        const QString request_id = nextRequestId(QStringLiteral("text"));
        const qint64 submitted_at_ms = QDateTime::currentMSecsSinceEpoch();
        request_started_at_.insert(request_id, submitted_at_ms);
        mode_value_->setText(QStringLiteral("文字输入"));
        output_value_->setText(speak_check_->isChecked() ? QStringLiteral("板端朗读")
                                                          : QStringLiteral("仅界面显示"));
        sendJson(QJsonObject{{QStringLiteral("type"), QStringLiteral("submit_text")},
                             {QStringLiteral("request_id"), request_id},
                             {QStringLiteral("text"), text},
                             {QStringLiteral("enable_tts"), speak_check_->isChecked()}});
        appendLog(QStringLiteral("信息"), QStringLiteral("文字输入"), QStringLiteral("已提交文字请求。"), submitted_at_ms);
        input_->clear();
    }

    QString nextRequestId(const QString& prefix) {
        return QStringLiteral("%1-%2").arg(prefix).arg(QDateTime::currentMSecsSinceEpoch());
    }

    void sendJson(const QJsonObject& object) {
        if (socket_.state() != QAbstractSocket::ConnectedState) return;
        socket_.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        socket_.write("\n");
    }

    void readMessages() {
        pending_bytes_.append(socket_.readAll());
        while (true) {
            const int line_end = pending_bytes_.indexOf('\n');
            if (line_end < 0) break;
            const QByteArray line = pending_bytes_.left(line_end);
            pending_bytes_.remove(0, line_end + 1);
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (document.isObject()) handleMessage(document.object());
        }
    }

    void handleMessage(const QJsonObject& message) {
        const QString type = message.value(QStringLiteral("type")).toString();
        const QString text = message.value(QStringLiteral("text")).toString();
        const QString request_id = message.value(QStringLiteral("request_id")).toString();
        const double raw_timestamp = message.value(QStringLiteral("timestamp_ms")).toDouble();
        const qint64 timestamp_ms = raw_timestamp > 0
            ? static_cast<qint64>(raw_timestamp)
            : QDateTime::currentMSecsSinceEpoch();

        if (type == QStringLiteral("status")) {
            board_value_->setText(boardName(message.value(QStringLiteral("board_status")).toString()));
            mode_value_->setText(modeName(message.value(QStringLiteral("mode")).toString()));
            const QString intent = message.value(QStringLiteral("intent")).toString();
            intent_value_->setText(intent.isEmpty() ? QStringLiteral("--") : intent);
            appendLog(QStringLiteral("信息"), QStringLiteral("状态"),
                      QStringLiteral("已获取服务状态快照。"), timestamp_ms);
            return;
        }

        if (type == QStringLiteral("mode_changed")) {
            const QString mode = message.value(QStringLiteral("mode")).toString();
            mode_value_->setText(modeName(mode));
            const bool enable_tts = message.value(QStringLiteral("enable_tts")).toBool();
            output_value_->setText(enable_tts ? QStringLiteral("板端朗读")
                                               : QStringLiteral("仅界面显示"));
            appendLog(QStringLiteral("信息"), QStringLiteral("输入模式"),
                      QStringLiteral("切换为%1。").arg(modeName(mode)), timestamp_ms);
        } else if (type == QStringLiteral("user_message")) {
            const QString source = message.value(QStringLiteral("source")).toString();
            conversation_->append(QStringLiteral("<b>我（%1）：</b>%2")
                                      .arg(sourceName(source), text.toHtmlEscaped()));
            ++turn_count_;
            turns_value_->setText(QString::number(turn_count_));
            if (!request_id.isEmpty() && !request_started_at_.contains(request_id)) {
                request_started_at_.insert(request_id, timestamp_ms);
            }
            appendLog(QStringLiteral("信息"), sourceName(source), QStringLiteral("收到用户输入。"), timestamp_ms);
        } else if (type == QStringLiteral("intent_result")) {
            const QString intent = message.value(QStringLiteral("intent")).toString();
            intent_value_->setText(intent.isEmpty() ? QStringLiteral("--") : intent);
            conversation_->append(QStringLiteral("<span style=\"color:#64748b\">意图：%1</span>")
                                      .arg(intent.toHtmlEscaped()));
            appendLog(QStringLiteral("信息"), QStringLiteral("意图"), intent, timestamp_ms);
        } else if (type == QStringLiteral("reply_delta")) {
            if (!reply_open_) {
                conversation_->append(QStringLiteral("<b>助手：</b>"));
                reply_open_ = true;
            }
            conversation_->moveCursor(QTextCursor::End);
            conversation_->insertPlainText(text);
        } else if (type == QStringLiteral("reply_final")) {
            if (reply_open_) conversation_->append(QString());
            reply_open_ = false;
            const qint64 started_at = request_started_at_.take(request_id);
            if (started_at > 0 && timestamp_ms >= started_at) {
                latency_value_->setText(QStringLiteral("%1 ms").arg(timestamp_ms - started_at));
            }
            appendLog(QStringLiteral("信息"), QStringLiteral("回复"), QStringLiteral("本轮回复完成。"), timestamp_ms);
        } else if (type == QStringLiteral("board_connection")) {
            const QString board_status = message.value(QStringLiteral("status")).toString();
            board_value_->setText(boardName(board_status));
            appendLog(board_status == QStringLiteral("connected") ? QStringLiteral("信息") : QStringLiteral("警告"),
                      QStringLiteral("开发板"),
                      board_status == QStringLiteral("connected") ? QStringLiteral("开发板已连接。")
                                                                      : QStringLiteral("开发板已断开。"),
                      timestamp_ms);
        } else if (type == QStringLiteral("accepted")) {
            appendLog(QStringLiteral("信息"), QStringLiteral("服务"), QStringLiteral("服务已接受请求。"), timestamp_ms);
        } else if (type == QStringLiteral("error")) {
            conversation_->append(QStringLiteral("<span style=\"color:#b00020\">错误：%1</span>")
                                      .arg(text.toHtmlEscaped()));
            appendLog(QStringLiteral("错误"), QStringLiteral("服务"), text, timestamp_ms);
        }
    }

    void appendLog(const QString& level, const QString& category, const QString& detail,
                   qint64 timestamp_ms = 0) {
        if (timestamp_ms <= 0) timestamp_ms = QDateTime::currentMSecsSinceEpoch();
        constexpr int kMaxLogRows = 500;
        if (event_table_->rowCount() >= kMaxLogRows) event_table_->removeRow(0);
        const int row = event_table_->rowCount();
        event_table_->insertRow(row);
        const QString time_text = QDateTime::fromMSecsSinceEpoch(timestamp_ms)
                                      .toString(QStringLiteral("HH:mm:ss.zzz"));
        const QList<QString> values{time_text, level, category, detail};
        QColor color(QStringLiteral("#334155"));
        if (level == QStringLiteral("错误")) color = QColor(QStringLiteral("#B42318"));
        else if (level == QStringLiteral("警告")) color = QColor(QStringLiteral("#A16207"));
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values[column]);
            item->setForeground(QBrush(color));
            event_table_->setItem(row, column, item);
        }
        event_table_->scrollToBottom();
        ++event_count_;
        event_count_value_->setText(QString::number(event_count_));
        statusBar()->showMessage(QStringLiteral("%1 · %2").arg(category, detail));
    }

    void updateSendButton() {
        send_button_->setEnabled(socket_.state() == QAbstractSocket::ConnectedState &&
                                 !input_->toPlainText().trimmed().isEmpty());
    }

    QTcpSocket socket_;
    QByteArray pending_bytes_;
    QHash<QString, qint64> request_started_at_;
    QLineEdit* host_edit_{nullptr};
    QSpinBox* port_spin_{nullptr};
    QPushButton* connect_button_{nullptr};
    QPushButton* reset_button_{nullptr};
    QLabel* service_value_{nullptr};
    QLabel* board_value_{nullptr};
    QLabel* mode_value_{nullptr};
    QLabel* intent_value_{nullptr};
    QLabel* output_value_{nullptr};
    QLabel* turns_value_{nullptr};
    QLabel* latency_value_{nullptr};
    QLabel* event_count_value_{nullptr};
    QTextEdit* conversation_{nullptr};
    QPlainTextEdit* input_{nullptr};
    QCheckBox* speak_check_{nullptr};
    QPushButton* send_button_{nullptr};
    QTableWidget* event_table_{nullptr};
    bool reply_open_{false};
    int turn_count_{0};
    int event_count_{0};
};

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    DesktopClientWindow window;
    window.show();
    return application.exec();
}
