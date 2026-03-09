#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "types.h"

class MessageBase;
class MessageFilter;
class MessageListenerBase;

class MessageQueue
{
public:
	typedef std::deque<std::shared_ptr<MessageBase>> MessageBufferType;

	static std::shared_ptr<MessageQueue> getInstance();

	~MessageQueue();

	void registerListener(MessageListenerBase* listener);
	void unregisterListener(MessageListenerBase* listener);

	MessageListenerBase* getListenerById(Id listenerId) const;

	void addMessageFilter(std::shared_ptr<MessageFilter> filter);

	void pushMessage(std::shared_ptr<MessageBase> message);
	void processMessage(std::shared_ptr<MessageBase> message, bool asNextTask);

	void startMessageLoopThreaded();
	void startMessageLoop();
	void stopMessageLoop();

	bool loopIsRunning() const;
	bool hasMessagesQueued() const;

	void setSendMessagesAsTasks(bool sendMessagesAsTasks);

private:
	MessageQueue();
	MessageQueue(const MessageQueue&) = delete;
	void operator=(const MessageQueue&) = delete;

	void processMessages();
	void sendMessage(std::shared_ptr<MessageBase> message);
	void sendMessageAsTask(std::shared_ptr<MessageBase> message, bool asNextTask) const;

	MessageBufferType m_messageBuffer;
	std::vector<MessageListenerBase*> m_listeners;
	std::vector<std::shared_ptr<MessageFilter>> m_filters;

	size_t m_currentListenerIndex;
	size_t m_listenersLength;

	bool m_loopIsRunning;
	bool m_threadIsRunning;

	mutable std::mutex m_messageBufferMutex;
	mutable std::mutex m_listenersMutex;
	mutable std::mutex m_loopMutex;
	mutable std::mutex m_threadMutex;

	std::condition_variable m_wakeupCV;
	std::mutex m_wakeupMutex;
	bool m_wakeupFlag;
	std::condition_variable m_threadStoppedCV;

	bool m_sendMessagesAsTasks;
};

#endif	  // MESSAGE_QUEUE_H
