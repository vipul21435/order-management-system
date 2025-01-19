// Author: Vipul Raj Jha
// College: IIIT Delhi

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <fstream>
#include <ctime>
#include <cstdint>

struct Logon
{
    std::string username;
    std::string password;
};

struct Logout
{
    std::string username;
};

enum class RequestType
{
    Unknown = 0,
    New = 1,
    Modify = 2,
    Cancel = 3
};

enum class ResponseType
{
    Unknown = 0,
    Accept = 1,
    Reject = 2,
};

struct OrderRequest
{
    int32_t m_symbolId;
    float m_price;
    uint32_t m_qty;
    char m_side; // possible values 'B' or 'S'
    uint32_t m_orderId;
    RequestType m_requestType; // Added to know the request type
    std::chrono::steady_clock::time_point m_receivedTime; // Added to keep track of received time
};

struct OrderResponse
{
    uint32_t m_orderId;
    ResponseType m_responseType;
    std::chrono::steady_clock::time_point m_responseTime; // when the response is received
};

class OrderManagement
{
public:
    OrderManagement()
    {
        // Initialize configurations
        m_perSecondLimit = 100; // Throttle limit, configurable
        m_orderSentInCurrentSecond = 0;
        m_isRunning = true;
        m_currentSecond = 0;

        // Configure allowed time period (example: 10 AM to 1 PM)
        // For simplicity, I use system_clock to get time of day
        m_startTimeInSeconds = 10 * 3600; // 10 AM
        m_endTimeInSeconds = 13 * 3600;   // 1 PM

        // Start the sender thread
        m_senderThread = std::thread(&OrderManagement::senderThreadFunction, this);
    }

    ~OrderManagement()
    {
        // Signal the sender thread to exit
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_isRunning = false;
            m_condition.notify_all();
        }
        if (m_senderThread.joinable())
        {
            m_senderThread.join();
        }
    }

    void onData(OrderRequest && request) {
        // This method receives order requests from upstream systems.
        // I provide implementation for this method.
        // This method should finally call send() if the order request is eligible for sending to exchange.
        // I am free to decide which thread executes this function.

        std::unique_lock<std::mutex> lock(m_mutex);

        // Get current time in seconds since midnight
        int currentTimeSeconds = getCurrentTimeInSeconds();

        if (currentTimeSeconds < m_startTimeInSeconds || currentTimeSeconds >= m_endTimeInSeconds)
        {
            // Outside allowed time period, reject the order
            std::cout << "Order " << request.m_orderId << " received outside allowed time period, rejecting.\n";
            return; // Reject silently or notify upstream system if needed
        }

        // Add the received time to the order
        request.m_receivedTime = std::chrono::steady_clock::now();

        // For Modify and Cancel requests, check if order is in the queue
        if (request.m_requestType == RequestType::Modify)
        {
            // Modify the order in the queue if it exists
            bool found = false;
            for (auto &queuedOrder : m_orderQueue)
            {
                if (queuedOrder.m_orderId == request.m_orderId)
                {
                    // Update price and qty
                    queuedOrder.m_price = request.m_price;
                    queuedOrder.m_qty = request.m_qty;
                    found = true;
                    std::cout << "Order " << request.m_orderId << " in queue modified.\n";
                    break;
                }
            }
            if (!found)
            {
                // Order not found in queue, cannot modify
                std::cout << "Modify request received for Order " << request.m_orderId << " not in queue.\n";
            }
        }
        else if (request.m_requestType == RequestType::Cancel)
        {
            // Remove the order from the queue if it exists
            auto it = m_orderQueue.begin();
            while (it != m_orderQueue.end())
            {
                if (it->m_orderId == request.m_orderId)
                {
                    m_orderQueue.erase(it);
                    std::cout << "Order " << request.m_orderId << " in queue canceled.\n";
                    break;
                }
                ++it;
            }
        }
        else if (request.m_requestType == RequestType::New)
        {
            // New order
            // Check if we can send immediately or need to queue
            updateCurrentSecond();
            if (m_orderSentInCurrentSecond < m_perSecondLimit)
            {
                // Can send immediately
                m_orderSentInCurrentSecond++;
                // Add to the sent orders map with send time
                uint32_t orderId = request.m_orderId;
                m_sentOrders[orderId] = OrderSentInfo{std::chrono::steady_clock::now()};
                // Send the order via sender thread
                m_readyToSendOrders.push_back(request);
                m_condition.notify_one();
            }
            else
            {
                // Exceeded per-second limit, queue the order
                m_orderQueue.push_back(request);
                std::cout << "Order " << request.m_orderId << " queued due to throttling.\n";
            }
        }
        else
        {
            std::cout << "Unknown request type for Order " << request.m_orderId << ".\n";
        }
    }

    // onData method gets invoked when the exchange sends the response back.
    void onData(OrderResponse && response) {
        // I have to provide implementation for this method.
        // This method will be called after the data is received from exchange and converted to OrderResponse object.
        // The response object needs to be processed.
        // I am free to decide which thread executes this function.

        auto now = std::chrono::steady_clock::now();

        uint32_t orderId = response.m_orderId;

        // Find the sent order to calculate latency
        std::unique_lock<std::mutex> lock(m_mutex);
        auto it = m_sentOrders.find(orderId);
        if (it != m_sentOrders.end())
        {
            auto sentTime = it->second.sentTime;
            auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(now - sentTime).count();

            // Dump response to persistent storage
            std::ofstream outfile;
            outfile.open("order_responses.txt", std::ios_base::app); // Append to file
            outfile << "OrderID: " << orderId << ", ResponseType: " << static_cast<int>(response.m_responseType)
                    << ", Latency(ms): " << latency << "\n";
            outfile.close();

            // Remove from sent orders map
            m_sentOrders.erase(it);
        }
        else
        {
            // Response received for order not found
            std::cout << "Received response for unknown OrderID " << orderId << ".\n";
        }
    }

    // send methods sends the request to exchange.
    void send(const OrderRequest& request) {
        // I assume that this method sends the request to exchange.
        // I DON'T have to provide implementation for this method.
        // This method is not thread safe.
        // I am free to decide which thread executes this function.

        // Simulate sending request
        std::cout << "Sending OrderID: " << request.m_orderId << " to exchange.\n";
    }

    void sendLogon() {
        // I assume that this method sends the logon message to exchange.
        // I DON'T have to provide implementation for this method.
        // This method is not thread safe.

        std::cout << "Sending Logon message to exchange.\n";
    }

    void sendLogout() {
        // I assume that this method sends the logout message to exchange.
        // I DON'T have to provide implementation for this method.
        // This method is not thread safe.

        std::cout << "Sending Logout message to exchange.\n";
    }

private:
    struct OrderSentInfo
    {
        std::chrono::steady_clock::time_point sentTime;
    };

    // Member variables
    std::thread m_senderThread;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::deque<OrderRequest> m_orderQueue; // Queue for throttled orders
    std::deque<OrderRequest> m_readyToSendOrders; // Orders ready to send immediately
    std::unordered_map<uint32_t, OrderSentInfo> m_sentOrders; // Map of sent orders to their send time
    int m_perSecondLimit;
    int m_orderSentInCurrentSecond;
    int m_currentSecond;
    bool m_isRunning;
    int m_startTimeInSeconds; // Allowed start time in seconds since midnight
    int m_endTimeInSeconds;   // Allowed end time in seconds since midnight

    void senderThreadFunction()
    {
        // This is the sender thread function that manages sending orders to exchange
        bool loggedIn = false;

        while (m_isRunning)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            updateCurrentSecond();

            int currentTimeSeconds = getCurrentTimeInSeconds();

            if (currentTimeSeconds >= m_startTimeInSeconds && currentTimeSeconds < m_endTimeInSeconds)
            {
                if (!loggedIn)
                {
                    // Send logon
                    sendLogon();
                    loggedIn = true;
                }

                // Send orders if any
                while (!m_readyToSendOrders.empty())
                {
                    const OrderRequest &request = m_readyToSendOrders.front();
                    send(request); // send() is not thread safe, but we are in the sender thread
                    m_readyToSendOrders.pop_front();
                }

                // Check if we have pending orders in the queue and can send more
                while (!m_orderQueue.empty() && m_orderSentInCurrentSecond < m_perSecondLimit)
                {
                    // Send next order
                    OrderRequest &request = m_orderQueue.front();
                    m_orderSentInCurrentSecond++;
                    // Add to the sent orders map with send time
                    uint32_t orderId = request.m_orderId;
                    m_sentOrders[orderId] = OrderSentInfo{std::chrono::steady_clock::now()};
                    send(request);
                    m_orderQueue.pop_front();
                }
            }
            else
            {
                if (loggedIn)
                {
                    // Send logout
                    sendLogout();
                    loggedIn = false;
                }
            }

            // Wait until the next second or when notified
            m_condition.wait_for(lock, std::chrono::milliseconds(100));
        }

        // When exiting, if logged in, send logout
        if (loggedIn)
        {
            sendLogout();
        }
    }

    int getCurrentTimeInSeconds()
    {
        // Get current time in seconds since midnight
        auto now = std::chrono::system_clock::now();
        std::time_t t_now = std::chrono::system_clock::to_time_t(now);
        struct tm * ptm = std::localtime(&t_now);
        int secondsSinceMidnight = ptm->tm_hour * 3600 + ptm->tm_min * 60 + ptm->tm_sec;
        return secondsSinceMidnight;
    }

    void updateCurrentSecond()
    {
        // Update current second and reset order count if new second
        auto now = std::chrono::steady_clock::now();
        int newCurrentSecond = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

        if (newCurrentSecond != m_currentSecond)
        {
            m_currentSecond = newCurrentSecond;
            m_orderSentInCurrentSecond = 0; // Reset counter
        }
    }

    // I can add any number of variables / functions as needed
};

/*
Assumptions:

- I assume that order IDs and quantities can be represented using 32-bit unsigned integers (uint32_t).
- Symbol IDs are 32-bit integers (int32_t).
- Prices are represented using 32-bit floats; I assume the precision is sufficient for the application.
- The system clock is accurate and can be used to determine the current time for the allowed time window.
- The maximum number of orders per second (throttling limit) is configured and set to 100.
- The exchange's responses can be matched to orders using the order ID.
- The processing of orders does not require precise microsecond timing.
- Latency is measured in milliseconds.
- The order management system runs on a reliable system without needing fault tolerance for this implementation.
- Output to "persistent storage" can be done via simple file I/O.

Author: Vipul Raj Jha
College: IIIT Delhi
*/