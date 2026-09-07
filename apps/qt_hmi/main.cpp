#include "chat_agent.h"
#include "conversation_runtime.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdlib>
#include <algorithm>
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

QString unixTimeText(int64_t timestamp) {
    if (timestamp <= 0) return QStringLiteral("--");
    return QDateTime::fromSecsSinceEpoch(timestamp).toString("yyyy-MM-dd HH:mm:ss");
}

QString csvField(QString value) {
    value.replace('"', QStringLiteral("\"\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

class BenchmarkCsvWriter final {
public:
    explicit BenchmarkCsvWriter(const QString& directory) {
        if (directory.isEmpty()) return;
        file_.setFileName(directory + QStringLiteral("/turn_metrics.csv"));
        if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            std::cerr << "[QtHmi] cannot open benchmark CSV: "
                      << file_.fileName().toStdString() << '\n';
            return;
        }
        stream_.setDevice(&file_);
        if (file_.size() == 0) {
            stream_ << "timestamp,request_id,input_text,intent,status,reply_kind,"
                       "intent_latency_ms,submit_to_first_reply_ms,submit_to_final_ms,"
                       "prompt_tokens,output_tokens,llm_ttft_ms,llm_prompt_decode_ms,"
                       "llm_decode_ms,llm_tokens_per_s,llm_truncated\n";
            stream_.flush();
        }
    }

    bool enabled() const { return file_.isOpen(); }

    void append(const QString& request_id, const QString& input, const QString& intent,
                const QString& status, const QString& reply_kind, qint64 intent_latency_ms,
                qint64 first_reply_ms, qint64 final_ms,
                const conversation::ConversationEvent& event) {
        if (!enabled()) return;
        stream_ << csvField(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)) << ','
                << csvField(request_id) << ',' << csvField(input) << ','
                << csvField(intent) << ',' << csvField(status) << ','
                << csvField(reply_kind) << ',' << intent_latency_ms << ','
                << first_reply_ms << ',' << final_ms << ','
                << event.prompt_tokens << ',' << event.output_tokens << ','
                << event.llm_ttft_ms << ',' << event.llm_prompt_decode_ms << ','
                << event.llm_decode_ms << ','
                << QString::number(event.llm_tokens_per_s, 'f', 2) << ','
                << (event.llm_truncated ? 1 : 0) << '\n';
        stream_.flush();
    }

private:
    QFile file_;
    QTextStream stream_;
};

struct ActiveTurn {
    bool active{false};
    QString request_id;
    QString input;
    QString intent;
    QElapsedTimer timer;
    qint64 first_reply_ms{-1};
};

class HomeHmi final : public QMainWindow {
public:
    explicit HomeHmi(conversation::ConversationRuntime& runtime, const QString& benchmark_dir)
        : runtime_(runtime), benchmark_writer_(benchmark_dir) {
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
                if (active_turn_.active && active_turn_.request_id ==
                                               QString::fromUtf8(event.request_id.c_str())) {
                    active_turn_.intent = intent;
                }
                break;
            case conversation::EventType::ToolResult:
                tool_value_->setText(event_text);
                break;
            case conversation::EventType::ReplyDelta:
                markFirstReply(event);
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
                finishTurn(event, QStringLiteral("ok"));
                refreshHistory();
                statusBar()->showMessage(QStringLiteral("本轮对话完成"));
                send_button_->setEnabled(true);
                break;
            case conversation::EventType::Error:
                finishTurn(event, QStringLiteral("error"));
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

        auto* metrics_box = new QGroupBox(QStringLiteral("本轮性能指标"), central);
        auto* metrics_layout = new QHBoxLayout(metrics_box);
        metrics_value_ = new QLabel(QStringLiteral("等待第一轮测试；可通过 --benchmark-dir 输出 CSV"),
                                    metrics_box);
        metrics_value_->setWordWrap(true);
        metrics_layout->addWidget(metrics_value_);
        layout->addWidget(metrics_box);

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

        auto* right_tabs = new QTabWidget(central);

        auto* log_page = new QWidget(right_tabs);
        auto* log_layout = new QVBoxLayout(log_page);
        event_log_ = new QPlainTextEdit(log_page);
        event_log_->setReadOnly(true);
        log_layout->addWidget(event_log_);
        right_tabs->addTab(log_page, QStringLiteral("运行事件"));

        auto* history_page = new QWidget(right_tabs);
        auto* history_layout = new QVBoxLayout(history_page);
        auto* history_hint = new QLabel(
            QStringLiteral("记录由现有对话接口自动持久化：可说“记住钥匙放在玄关”，"
                           "“我喜欢 26 度”，或“客厅灯坏了”。"),
            history_page);
        history_hint->setWordWrap(true);
        history_hint->setStyleSheet(QStringLiteral("color: #64748B;"));
        history_layout->addWidget(history_hint);
        auto* refresh_history_button = new QPushButton(QStringLiteral("刷新记录"), history_page);
        history_layout->addWidget(refresh_history_button, 0, Qt::AlignLeft);

        auto* history_tabs = new QTabWidget(history_page);
        object_locations_table_ = createHistoryTable(
            {QStringLiteral("物品"), QStringLiteral("属性"), QStringLiteral("位置"),
             QStringLiteral("更新时间"), QStringLiteral("操作")}, history_tabs);
        preferences_table_ = createHistoryTable(
            {QStringLiteral("主题"), QStringLiteral("属性"), QStringLiteral("偏好值"),
             QStringLiteral("更新时间"), QStringLiteral("操作")}, history_tabs);
        device_faults_table_ = createHistoryTable(
            {QStringLiteral("房间"), QStringLiteral("设备"), QStringLiteral("故障类型"),
             QStringLiteral("描述"), QStringLiteral("发生时间"), QStringLiteral("操作")},
            history_tabs);
        history_tabs->addTab(object_locations_table_, QStringLiteral("物品位置"));
        history_tabs->addTab(preferences_table_, QStringLiteral("用户偏好"));
        history_tabs->addTab(device_faults_table_, QStringLiteral("设备故障"));
        history_layout->addWidget(history_tabs, 1);
        right_tabs->addTab(history_page, QStringLiteral("历史记忆"));

        content_layout->addWidget(right_tabs, 2);
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
        connect(refresh_history_button, &QPushButton::clicked, this, [this] {
            refreshHistory();
            statusBar()->showMessage(QStringLiteral("历史记录已刷新"));
        });
        refreshHistory();
    }

    void submitText(const QString& raw_text) {
        const QString text = raw_text.trimmed();
        if (text.isEmpty()) return;

        conversation::ConversationRequest request;
        request.text = text.toUtf8().constData();
        request.source = conversation::InputSource::Text;
        request.enable_tts = false;  // 当前文字验证不依赖音频硬件。
        request.request_id = std::to_string(++request_counter_);
        const QString request_id = QString::fromStdString(request.request_id);
        if (runtime_.submit(std::move(request)) == 0) {
            statusBar()->showMessage(QStringLiteral("运行时未启动，无法提交文字"));
            return;
        }
        input_->clear();
        active_turn_.active = true;
        active_turn_.request_id = request_id;
        active_turn_.input = text;
        active_turn_.intent.clear();
        active_turn_.first_reply_ms = -1;
        active_turn_.timer.start();
        send_button_->setEnabled(false);
        source_value_->setText(QStringLiteral("文字输入（本地进程）"));
        intent_value_->setText(QStringLiteral("处理中"));
        tool_value_->setText(QStringLiteral("等待路由"));
        metrics_value_->setText(QStringLiteral("处理中：等待意图与模型结果"));
        statusBar()->showMessage(QStringLiteral("正在处理文字请求"));
    }

    void markFirstReply(const conversation::ConversationEvent& event) {
        if (!active_turn_.active || active_turn_.request_id !=
                                        QString::fromUtf8(event.request_id.c_str()) ||
            active_turn_.first_reply_ms >= 0) {
            return;
        }
        active_turn_.first_reply_ms = active_turn_.timer.elapsed();
    }

    void finishTurn(const conversation::ConversationEvent& event, const QString& status) {
        if (!active_turn_.active || active_turn_.request_id !=
                                        QString::fromUtf8(event.request_id.c_str())) {
            return;
        }

        const qint64 final_ms = active_turn_.timer.elapsed();
        const qint64 intent_ms = event.intent_latency_ms;
        const QString intent = active_turn_.intent.isEmpty()
            ? QString::fromUtf8(event.intent.c_str()) : active_turn_.intent;
        const QString reply_kind = event.has_llm_metrics
            ? QStringLiteral("llm_generation") : QStringLiteral("fixed_or_error");

        if (event.has_llm_metrics) {
            metrics_value_->setText(
                QStringLiteral("意图 %1 ms ｜ 首回复 %2 ms ｜ TTFT %3 ms ｜ "
                               "prefill %4 ms ｜ 生成 %5 tokens / %6 ms（%7 tok/s）")
                    .arg(intent_ms)
                    .arg(active_turn_.first_reply_ms)
                    .arg(event.llm_ttft_ms)
                    .arg(event.llm_prompt_decode_ms)
                    .arg(event.output_tokens)
                    .arg(event.llm_decode_ms)
                    .arg(event.llm_tokens_per_s, 0, 'f', 2));
        } else {
            metrics_value_->setText(
                QStringLiteral("意图 %1 ms ｜ 首回复 %2 ms ｜ 完成 %3 ms ｜ %4")
                    .arg(intent_ms)
                    .arg(active_turn_.first_reply_ms)
                    .arg(final_ms)
                    .arg(status == QStringLiteral("ok")
                             ? QStringLiteral("规则回复，未调用聊天生成")
                             : QStringLiteral("处理失败")));
        }

        benchmark_writer_.append(active_turn_.request_id, active_turn_.input, intent,
                                 status, reply_kind, intent_ms,
                                 active_turn_.first_reply_ms, final_ms, event);
        active_turn_.active = false;
    }

    void appendConversation(const QString& speaker, const QString& text) {
        conversation_view_->appendPlainText(speaker + QStringLiteral("：") + text);
    }

    void appendLog(const QString& type, const QString& text) {
        event_log_->appendPlainText(nowText() + QStringLiteral(" [") + type +
                                    QStringLiteral("] ") + text);
    }

    static QTableWidget* createHistoryTable(const QStringList& headers, QWidget* parent) {
        auto* table = new QTableWidget(parent);
        table->setColumnCount(headers.size());
        table->setHorizontalHeaderLabels(headers);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->setVisible(false);
        for (int column = 0; column < headers.size(); ++column) {
            table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
        }
        return table;
    }

    static void setCell(QTableWidget* table, int row, int column, const QString& value) {
        auto* item = new QTableWidgetItem(value);
        item->setToolTip(value);
        table->setItem(row, column, item);
    }

    void deleteMemoryFromHistory(const assistant::MemoryItem& item) {
        const QString details = QStringLiteral("%1：%2 = %3")
                                    .arg(QString::fromUtf8(item.category.c_str()),
                                         QString::fromUtf8(item.subject.c_str()),
                                         QString::fromUtf8(item.value.c_str()));
        if (QMessageBox::question(this, QStringLiteral("删除历史记忆"),
                                  QStringLiteral("确定删除这条记录吗？\n%1\n\n删除后无法恢复。")
                                      .arg(details),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        if (!runtime_.deleteMemoryRecord(item)) {
            QMessageBox::warning(this, QStringLiteral("删除失败"),
                                 QStringLiteral("记录可能已被更新或删除，请刷新后重试。"));
            refreshHistory();
            return;
        }
        refreshHistory();
        statusBar()->showMessage(QStringLiteral("已删除历史记忆"), 3000);
    }

    void deleteFaultFromHistory(const assistant::DeviceEvent& event) {
        const QString details = QStringLiteral("%1 %2：%3")
                                    .arg(QString::fromUtf8(event.room.c_str()),
                                         QString::fromUtf8(event.device.c_str()),
                                         QString::fromUtf8(event.description.c_str()));
        if (QMessageBox::question(this, QStringLiteral("删除设备故障记录"),
                                  QStringLiteral("确定删除这条故障记录吗？\n%1\n\n删除后无法恢复。")
                                      .arg(details),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        if (!runtime_.deleteDeviceFaultRecord(event)) {
            QMessageBox::warning(this, QStringLiteral("删除失败"),
                                 QStringLiteral("记录可能已被更新或删除，请刷新后重试。"));
            refreshHistory();
            return;
        }
        refreshHistory();
        statusBar()->showMessage(QStringLiteral("已删除设备故障记录"), 3000);
    }

    void refreshHistory() {
        auto memories = runtime_.memorySnapshot();
        auto events = runtime_.eventSnapshot();
        std::sort(memories.begin(), memories.end(), [](const auto& left, const auto& right) {
            return left.updated_at > right.updated_at;
        });
        std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
            return left.timestamp > right.timestamp;
        });

        object_locations_table_->setRowCount(0);
        preferences_table_->setRowCount(0);
        device_faults_table_->setRowCount(0);
        for (const auto& item : memories) {
            QTableWidget* table = nullptr;
            if (item.category == "OBJECT_LOCATION") {
                table = object_locations_table_;
            } else if (item.category == "USER_PREFERENCE") {
                table = preferences_table_;
            } else {
                continue;
            }
            const int row = table->rowCount();
            table->insertRow(row);
            setCell(table, row, 0, QString::fromUtf8(item.subject.c_str()));
            setCell(table, row, 1, QString::fromUtf8(item.attribute.c_str()));
            setCell(table, row, 2, QString::fromUtf8(item.value.c_str()));
            setCell(table, row, 3, unixTimeText(item.updated_at));
            auto* delete_button = new QPushButton(QStringLiteral("删除"), table);
            delete_button->setToolTip(QStringLiteral("删除这条历史记忆"));
            connect(delete_button, &QPushButton::clicked, this,
                    [this, item] { deleteMemoryFromHistory(item); });
            table->setCellWidget(row, 4, delete_button);
        }
        for (const auto& event : events) {
            const int row = device_faults_table_->rowCount();
            device_faults_table_->insertRow(row);
            setCell(device_faults_table_, row, 0, QString::fromUtf8(event.room.c_str()));
            setCell(device_faults_table_, row, 1, QString::fromUtf8(event.device.c_str()));
            setCell(device_faults_table_, row, 2, QString::fromUtf8(event.event_type.c_str()));
            setCell(device_faults_table_, row, 3, QString::fromUtf8(event.description.c_str()));
            setCell(device_faults_table_, row, 4, unixTimeText(event.timestamp));
            auto* delete_button = new QPushButton(QStringLiteral("删除"), device_faults_table_);
            delete_button->setToolTip(QStringLiteral("删除这条设备故障记录"));
            connect(delete_button, &QPushButton::clicked, this,
                    [this, event] { deleteFaultFromHistory(event); });
            device_faults_table_->setCellWidget(row, 5, delete_button);
        }
    }

    conversation::ConversationRuntime& runtime_;
    QPlainTextEdit* conversation_view_{nullptr};
    QPlainTextEdit* input_{nullptr};
    QPlainTextEdit* event_log_{nullptr};
    QLabel* source_value_{nullptr};
    QLabel* intent_value_{nullptr};
    QLabel* tool_value_{nullptr};
    QLabel* metrics_value_{nullptr};
    QTableWidget* object_locations_table_{nullptr};
    QTableWidget* preferences_table_{nullptr};
    QTableWidget* device_faults_table_{nullptr};
    QPushButton* send_button_{nullptr};
    BenchmarkCsvWriter benchmark_writer_;
    ActiveTurn active_turn_;
    bool reply_open_{false};
    uint64_t request_counter_{0};
};

bool parseArguments(int argc, char* argv[], std::string& model_path,
                    std::string& state_dir, std::string& benchmark_dir) {
    if (argc < 2) return false;
    model_path = argv[1];
    state_dir = "runtime";
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--state-dir" && i + 1 < argc) {
            state_dir = argv[++i];
        } else if (option == "--benchmark-dir" && i + 1 < argc) {
            benchmark_dir = argv[++i];
        } else {
            return false;
        }
    }
    return true;
}

void printUsage(const char* binary) {
    std::cerr << "Usage: " << binary
              << " <llm_model.gguf> [--state-dir <directory>]"
                 " [--benchmark-dir <directory>]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string model_path;
    std::string state_dir;
    std::string benchmark_dir;
    if (!parseArguments(argc, argv, model_path, state_dir, benchmark_dir)) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    if (agent_init(model_path.c_str(), nullptr) != 0) {
        std::cerr << "[QtHmi] Gemma initialization failed: " << model_path << '\n';
        return EXIT_FAILURE;
    }

    std::filesystem::create_directories(state_dir);
    if (!benchmark_dir.empty()) std::filesystem::create_directories(benchmark_dir);
    conversation::ConversationRuntime runtime(state_dir + "/assistant_memory_v2.tsv",
                                              state_dir + "/device_fault_events.tsv");
    if (!runtime.initialize() || !runtime.start()) {
        std::cerr << "[QtHmi] Conversation runtime initialization failed\n";
        agent_destroy();
        return EXIT_FAILURE;
    }

    QApplication app(argc, argv);
    HomeHmi window(runtime, QString::fromStdString(benchmark_dir));
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
