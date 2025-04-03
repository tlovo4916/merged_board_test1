# WebSocket服务端源码

import com.alibaba.fastjson.JSON;
import com.alibaba.fastjson.JSONObject;
import lombok.extern.slf4j.Slf4j;
import org.springframework.stereotype.Component;

import javax.websocket.OnClose;
import javax.websocket.OnMessage;
import javax.websocket.OnOpen;
import javax.websocket.Session;
import javax.websocket.server.PathParam;
import javax.websocket.server.ServerEndpoint;
import java.io.IOException;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;

@Slf4j
@Component
@ServerEndpoint("/robws/{clientId}")
public class RobotsWebSocket {

    private static final ConcurrentHashMap<String, Session> sessionMap = new ConcurrentHashMap<String, Session>();

    @OnOpen
    public void onOpen(Session session, @PathParam("clientId") String clientId) {
        sessionMap.put(clientId, session); // 存储客户端ID与会话的映射[8,12](@ref)
        log.info("客户端 {} 连接成功，当前在线数：{}", clientId, sessionMap.size());
    }

    @OnClose
    public void onClose(Session session) {
        String clientId = session.getPathParameters().getOrDefault("clientId", null);
        sessionMap.remove(clientId);
        log.info("客户端 [{}] 断开连接", clientId);
    }

    @OnMessage
    public void onMessage(Session session, String message) {
        String clientId = session.getPathParameters().getOrDefault("clientId", null);
        log.info("收到客户端 {} 的消息：{}", clientId, message);
        JSONObject jsonObject = JSONObject.parseObject(message);
        String eventName = jsonObject.getString("event");
        Object param = jsonObject.get("data");
        log.warn("[{}][{}] ==> {}", clientId, eventName, JSON.toJSONString(param));
    }

    public static List<String> getClientIdList() {
        List<String> clientIdList = new ArrayList<>();
        sessionMap.forEach((key, value) -> {
            clientIdList.add(key);
        });
        return clientIdList;
    }

    /**
     * 定向触发客户端事件的方法
     * @param clientId 客户端ID
     * @param eventName 客户端事件名称
     * @param param 事件参数
     * @throws IOException
     */
    public static void triggerEvent(String clientId, String eventName, Object param) throws IOException {
        Session targetSession = sessionMap.get(clientId);
        if (targetSession != null && targetSession.isOpen()) {
            // 使用结构化的 JSON 对象构建（避免手动拼接）
            JSONObject jsonPayload = new JSONObject();
            jsonPayload.put("event", eventName);
            jsonPayload.put("data", param); // 依赖 JSON 库的深度序列化能力
            // 线程安全发送（同步块保护）
            synchronized (targetSession) {
                targetSession.getBasicRemote().sendText(jsonPayload.toJSONString());
            }
        }
    }

}