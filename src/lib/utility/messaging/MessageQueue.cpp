#include "MessageQueue.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "MessageBase.h"
#include "MessageFilter.h"
#include "MessageListenerBase.h"
#include "TabId.h"
#include "TaskGroupParallel.h"
#include "TaskGroupSequence.h"
#include "TaskLambda.h"
#include "logging.h"

std::shared_ptr<MessageQueue> MessageQueue::getInstance()
{
	// Static local: guaranteed thread-safe initialization in C++11+ (no race on first call)
	static std::shared_ptr<MessageQueue> instance(new MessageQueue());
	return instance;
}

MessageQueue::~MessageQueue()
{
	std::lock_guard<std::mutex> lock(m_listenersMutex);
	for (size_t i = 0; i < m_listeners.size(); i++)
	{
		m_listeners[i]->removedListener();
	}
	m_listeners.clear();
}

void MessageQueue::registerListener(MessageListenerBase* listener)
{
	std::lock_guard<std::mutex> lock(m_listenersMutex);
	m_listeners.push_back(listener);
}

void MessageQueue::unregisterListener(MessageListenerBase* listener)
{
	std::lock_guard<std::mutex> lock(m_listenersMutex);
	for (size_t i = 0; i < m_listeners.size(); i++)
	{
		if (m_listeners[i] == listener)
		{
			m_listeners.erase(m_listeners.begin() + i);

			// m_currentListenerIndex and m_listenersLength need to be updated in case this happens
			// while a message is handled.
			if (i <= m_currentListenerIndex)
			{
				m_currentListenerIndex--;
			}

			if (i < m_listenersLength)
			{
				m_listenersLength--;
			}

			return;
		}
	}

	LOG_ERROR("Listener was not found");
}

MessageListenerBase* MessageQueue::getListenerById(Id listenerId) const
{
	std::lock_guard<std::mutex> lock(m_listenersMutex);
	for (size_t i = 0; i < m_listeners.size(); i++)
	{
		if (m_listeners[i]->getId() == listenerId)
		{
			return m_listeners[i];
		}
	}
	return nullptr;
}

void MessageQueue::addMessageFilter(std::shared_ptr<MessageFilter> filter)
{
	m_filters.push_back(filter);
}

void MessageQueue::pushMessage(std::shared_ptr<MessageBase> message)
{
	{
		std::lock_guard<std::mutex> lock(m_messageBufferMutex);
		m_messageBuffer.push_back(message);
	}
	{
		std::lock_guard<std::mutex> lock(m_wakeupMutex);
		m_wakeupFlag = true;
	}
	m_wakeupCV.notify_one();
}

void MessageQueue::processMessage(std::shared_ptr<MessageBase> message, bool asNextTask)
{
	if (message->isLogged())
	{
		LOG_INFO_BARE(L"send " + message->str());
	}

	if (m_sendMessagesAsTasks && message->sendAsTask())
	{
		sendMessageAsTask(message, asNextTask);
	}
	else
	{
		sendMessage(message);
	}
}

void MessageQueue::startMessageLoopThreaded()
{
	std::thread(&MessageQueue::startMessageLoop, this).detach();

	std::lock_guard<std::mutex> lock(m_threadMutex);
	m_threadIsRunning = true;
}

void MessageQueue::startMessageLoop()
{
	{
		std::lock_guard<std::mutex> lock(m_loopMutex);

		if (m_loopIsRunning)
		{
			LOG_ERROR("Loop is already running");
			return;
		}

		m_loopIsRunning = true;
	}

	while (true)
	{
		processMessages();

		{
			std::lock_guard<std::mutex> lock(m_loopMutex);

			if (!m_loopIsRunning)
			{
				break;
			}
		}

		{
			std::unique_lock<std::mutex> lock(m_wakeupMutex);
			m_wakeupCV.wait_for(
				lock, std::chrono::milliseconds(25), [this] { return m_wakeupFlag; });
			m_wakeupFlag = false;
		}
	}

	{
		std::lock_guard<std::mutex> lock(m_threadMutex);
		m_threadIsRunning = false;
	}
	m_threadStoppedCV.notify_all();
}

void MessageQueue::stopMessageLoop()
{
	{
		std::lock_guard<std::mutex> lock(m_loopMutex);

		if (!m_loopIsRunning)
		{
			LOG_WARNING("Loop is not running");
		}

		m_loopIsRunning = false;
	}

	{
		std::lock_guard<std::mutex> lock(m_wakeupMutex);
		m_wakeupFlag = true;
	}
	m_wakeupCV.notify_all();

	{
		std::unique_lock<std::mutex> lock(m_threadMutex);
		m_threadStoppedCV.wait(lock, [this] { return !m_threadIsRunning; });
	}
}

bool MessageQueue::loopIsRunning() const
{
	std::lock_guard<std::mutex> lock(m_loopMutex);
	return m_loopIsRunning;
}

bool MessageQueue::hasMessagesQueued() const
{
	std::lock_guard<std::mutex> lock(m_messageBufferMutex);
	return m_messageBuffer.size() > 0;
}

void MessageQueue::setSendMessagesAsTasks(bool sendMessagesAsTasks)
{
	m_sendMessagesAsTasks = sendMessagesAsTasks;
}


MessageQueue::MessageQueue()
	: m_currentListenerIndex(0)
	, m_listenersLength(0)
	, m_loopIsRunning(false)
	, m_threadIsRunning(false)
	, m_sendMessagesAsTasks(false)
	, m_wakeupFlag(false)
{
}

void MessageQueue::processMessages()
{
	while (true)
	{
		std::shared_ptr<MessageBase> message;
		{
			std::lock_guard<std::mutex> lock(m_messageBufferMutex);

			for (std::shared_ptr<MessageFilter> filter: m_filters)
			{
				if (!m_messageBuffer.size())
				{
					break;
				}

				filter->filter(&m_messageBuffer);
			}

			if (!m_messageBuffer.size())
			{
				break;
			}

			message = m_messageBuffer.front();
			m_messageBuffer.pop_front();
		}

		processMessage(message, false);
	}
}

void MessageQueue::sendMessage(std::shared_ptr<MessageBase> message)
{
	// Snapshot matching listeners under the lock so we never call external code while holding it.
	// This prevents use-after-free when a listener unregisters (and is destroyed) during dispatch.
	std::vector<MessageListenerBase*> matching;
	{
		std::lock_guard<std::mutex> lock(m_listenersMutex);
		m_listenersLength = m_listeners.size();
		for (m_currentListenerIndex = 0; m_currentListenerIndex < m_listenersLength;
			 m_currentListenerIndex++)
		{
			MessageListenerBase* listener = m_listeners[m_currentListenerIndex];
			if (listener->getType() == message->getType() &&
				(message->getSchedulerId() == 0 || listener->getSchedulerId() == 0 ||
				 listener->getSchedulerId() == message->getSchedulerId()))
			{
				matching.push_back(listener);
			}
		}
	}

	for (MessageListenerBase* listener: matching)
	{
		// Re-check that the listener is still registered before dispatching; it may have
		// unregistered (and been destroyed) between the snapshot and now.
		{
			std::lock_guard<std::mutex> lock(m_listenersMutex);
			if (std::find(m_listeners.begin(), m_listeners.end(), listener) == m_listeners.end())
			{
				continue;
			}
		}
		listener->handleMessageBase(message.get());
	}
}

void MessageQueue::sendMessageAsTask(std::shared_ptr<MessageBase> message, bool asNextTask) const
{
	std::shared_ptr<TaskGroup> taskGroup;
	if (message->isParallel())
	{
		taskGroup = std::make_shared<TaskGroupParallel>();
	}
	else
	{
		taskGroup = std::make_shared<TaskGroupSequence>();
	}

	{
		std::lock_guard<std::mutex> lock(m_listenersMutex);
		for (size_t i = 0; i < m_listeners.size(); i++)
		{
			MessageListenerBase* listener = m_listeners[i];

			if (listener->getType() == message->getType() &&
				(message->getSchedulerId() == 0 || listener->getSchedulerId() == 0 ||
				 listener->getSchedulerId() == message->getSchedulerId()))
			{
				Id listenerId = listener->getId();
				taskGroup->addTask(std::make_shared<TaskLambda>([listenerId, message]() {
					MessageListenerBase* listener = MessageQueue::getInstance()->getListenerById(
						listenerId);
					if (listener)
					{
						listener->handleMessageBase(message.get());
					}
				}));
			}
		}
	}

	Id schedulerId = message->getSchedulerId();
	if (!schedulerId)
	{
		schedulerId = TabId::app();
	}

	if (asNextTask)
	{
		Task::dispatchNext(schedulerId, taskGroup);
	}
	else
	{
		Task::dispatch(schedulerId, taskGroup);
	}
}
