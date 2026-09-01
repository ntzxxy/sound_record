#include "chat_agent.h"
#include "conversation_runtime.h"

#include <QApplication>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

QString eventName(conversation::EventType type) {
    return QString::fromUtf8(conversation::toString(type));
}

QString nowText() {
    return QDateTime::currentDateTime().toString("HH:mm:ss");
}

class HomeHmi final : public QMainWindow {
public:
    explicit HomeHmi(conversation::ConversationRuntime& runtime)
        : runtime_(runtime) {
        setWindowTitle(QStringLiteral("边端智能家居文字控制台"));
        resize(1080, 720);
        buildUi();
    }

    void onConversationEvent(const conversation::ConversationEvent& event) {
        const QString event_text = QString::fromUtf8(event.text.c_str());
        const QString intent = QString::fromUtf8(event.intent.c_str());
        appendLog(eventName(event.type), event_text);

        switch (event.type) {
            case conversation::EventType::ModeChanged:
                source_value_->setText(QStringLiteral("文字输入（本地进程）"));
                break;
            case conversation::EventType::UserMessage:
                appendConversation(QStringLiteral("用户"), event_text);
                break;
            case conversation::EventType::IntentResult:
                intent_value_->setText(intent.isEmpty() ? QStringLiteral("未分类") : intent);
                break;
            case conversation::EventType::ToolResult:
                tool_value_->setText(event_text);
                break;
            case conversation::EventType::ReplyDelta:
                if (!reply_open_) {
                    conversation_view_->appendPlainText(QStringLiteral("助手：") + event_text);
                    reply_open_ = true;
                } else {
                    conversation_view_->insertPlainText(event_text);
                }
                break;
            case conversation::EventType::ReplyFinal:
                if (reply_open_) conversation_view_->appendPlainText(QString());
                reply_open_ = false;
                statusBar()->showMessage(QStringLiteral("本轮对话完成"));
                send_button_->setEnabled(true);
                break;
            case conversation::EventType::Error:
                statusBar()->showMessage(QStringLiteral("对话处理失败：") + event_text);
                send_button_->setEnabled(true);
                break;
        }
    }

private:
    void buildUi() {
        auto* central = new QWidget(this);
        auto* layout = new QVBoxLayout(central);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->setSpacing(12);

        auto* title = new QLabel(QStringLiteral("边端智能家居文字控制台"), central);
        title->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 700; color: #123B62;"));
        layout->addWidget(title);

        auto* summary = new QLabel(
            QStringLiteral("Qt → ConversationRuntime → 家居路由 / Gemma → 事件回调；不使用 TCP 服务。"),
            central);
        summary->setStyleSheet(QStringLiteral("color: #64748B;"));
        layout->addWidget(summary);

        auto* state_box = new QGroupBox(QStringLiteral("本轮状态"), central);
        auto* state_layout = new QHBoxLayout(state_box);
        source_value_ = new QLabel(QStringLiteral("等待文字输入"), state_box);
        intent_value_ = new QLabel(QStringLiteral("--"), state_box);
        tool_value_ = new QLabel(QStringLiteral("--"), state_box);
        tool_value_->setWordWrap(true);
        state_layout->addWidget(new QLabel(QStringLiteral("输入："), state_box));
        state_layout->addWidget(source_value_, 1);
        state_layout->addWidget(new QLabel(QStringLiteral("意图："), state_box));
        state_layout->addWidget(intent_value_, 1);
        state_layout->addWidget(new QLabel(QStringLiteral("模拟执行："), state_box));
        state_layout->addWidget(tool_value_, 3);
        layout->addWidget(state_box);

        auto* content_layout = new QHBoxLayout();
        auto* conversation_box = new QGroupBox(QStringLiteral("文字对话"), central);
        auto* conversation_layout = new QVBoxLayout(conversation_box);
        conversation_view_ = new QPlainTextEdit(conversation_box);
        conversation_view_->setReadOnly(true);
        conversation_view_->setPlaceholderText(QStringLiteral("Gemma 的流式回复将显示在这里。"));
        conversation_layout->addWidget(conversation_view_, 1);
        input_ = new QPlainTextEdit(conversation_box);
        input_->setFixedHeight(82);
        input_->setPlaceholderText(QStringLiteral("例如：打开客厅灯；或输入任意问题与 Gemma 对话。"));
        conversation_layout->addWidget(input_);
        auto* buttons = new QHBoxLayout();
        auto* reset_button = new QPushButton(QStringLiteral("新建会话"), conversation_box);
        send_button_ = new QPushButton(QStringLiteral("发送文字"), conversation_box);
        send_button_->setDefault(true);
        buttons->addWidget(reset_button);
        buttons->addStretch();
        buttons->addWidget(send_button_);
        conversation_layout->addLayout(buttons);
        content_layout->addWidget(conversation_box, 3);

        auto* log_box = new QGroupBox(QStringLiteral("运行事件"), central);
        auto* log_layout = new QVBoxLayout(log_box);
        event_log_ = new QPlainTextEdit(log_box);
        event_log_->setReadOnly(true);
        log_layout->addWidget(event_log_);
        content_layout->addWidget(log_box, 2);
        layout->addLayout(content_layout, 1);

        setCentralWidget(central);
        statusBar()->showMessage(QStringLiteral("就绪：等待文字输入"));
        connect(send_button_, &QPushButton::clicked, this, [this] {
            submitText(input_->toPlainText());
        });
        connect(reset_button, &QPushButton::clicked, this, [this] {
            runtime_.resetConversation();
            conversation_view_->appendPlainText(QStringLiteral("—— 已新建会话 ——"));
            statusBar()->showMessage(QStringLiteral("会话已重置"));
        });
    }

    void submitText(const QString& raw_text) {
        const QString text = raw_text.trimmed();
        if (text.isEmpty()) return;

        conversation::ConversationRequest request;
        request.text = text.toUtf8().constData();
        request.source = conversation::InputSource::Text;
        request.enable_tts = false;  // 当前文字验证不依赖音频硬件。
        request.request_id = std::to_string(++request_counter_);
        if (runtime_.submit(std::move(request)) == 0) {
            statusBar()->showMessage(QStringLiteral("运行时未启动，无法提交文字"));
            return;
        }
        input_->clear();
        send_button_->setEnabled(false);
        source_value_->setText(QStringLiteral("文字输入（本地进程）"));
        intent_value_->setText(QStringLiteral("处理中"));
        tool_value_->setText(QStringLiteral("等待路由"));
        statusBar()->showMessage(QStringLiteral("正在处理文字请求"));
    }

    void appendConversation(const QString& speaker, const QString& text) {
        conversation_view_->appendPlainText(speaker + QStringLiteral("：") + text);
    }

    void appendLog(const QString& type, const QString& text) {
        event_log_->appendPlainText(nowText() + QStringLiteral(" [") + type +
                                    QStringLiteral("] ") + text);
    }

    conversation::ConversationRuntime& runtime_;
    QPlainTextEdit* conversation_view_{nullptr};
    QPlainTextEdit* input_{nullptr};
    QPlainTextEdit* event_log_{nullptr};
    QLabel* source_value_{nullptr};
    QLabel* intent_value_{nullptr};
    QLabel* tool_value_{nullptr};
    QPushButton* send_button_{nullptr};
    bool reply_open_{false};
    uint64_t request_counter_{0};
};

bool parseArguments(int argc, char* argv[], std::string& model_path,
                    std::string& state_dir) {
    if (argc < 2) return false;
    model_path = argv[1];
    state_dir = "runtime";
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--state-dir" && i + 1 < argc) {
            state_dir = argv[++i];
        } else {
            return false;
        }
    }
    return true;
}

void printUsage(const char* binary) {
    std::cerr << "Usage: " << binary
              << " <llm_model.gguf> [--state-dir <directory>]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string model_path;
    std::string state_dir;
    if (!parseArguments(argc, argv, model_path, state_dir)) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    if (agent_init(model_path.c_str(), nullptr) != 0) {
        std::cerr << "[QtHmi] Gemma initialization failed: " << model_path << '\n';
        return EXIT_FAILURE;
    }

    std::filesystem::create_directories(state_dir);
    conversation::ConversationRuntime runtime(state_dir + "/assistant_memory_v2.tsv",
                                              state_dir + "/device_fault_events.tsv");
    if (!runtime.initialize() || !runtime.start()) {
        std::cerr << "[QtHmi] Conversation runtime initialization failed\n";
        agent_destroy();
        return EXIT_FAILURE;
    }

    QApplication app(argc, argv);
    HomeHmi window(runtime);
    runtime.setEventCallback([&window](const conversation::ConversationEvent& event) {
        QMetaObject::invokeMethod(&window, [&window, event] {
            window.onConversationEvent(event);
        }, Qt::QueuedConnection);
    });

    window.show();

    const int result = app.exec();
    runtime.setEventCallback({});
    runtime.stop();
    agent_destroy();
    return result;
}
