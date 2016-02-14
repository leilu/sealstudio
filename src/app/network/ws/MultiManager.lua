MultiManager = class("MultiManager")

function MultiManager:connect(param)
	local wsUrl = string.format("%s/%d/%d",NetWorkConst.WS.URL,8080,123)
	WebSocketManager:connect(wsUrl,param, self.onConnect,self.onMessage,self.onClose,self.onError)
end

function MultiManager:createRoom(roomName)
    local param = {}
    param.c = NetWorkConst.WS.MSG_CODE.CREATE_ROOM
    param.v = roomName
	MultiManager:emit(param)
end

function MultiManager:joinRoom(roomName)
	local param = {}
	param.c = NetWorkConst.WS.MSG_CODE.JOIN_ROOM
	param.v = roomName
	MultiManager:emit(param)
end

function MultiManager:leaveRoom()
	local param = {}
	param.c = NetWorkConst.WS.MSG_CODE.LEAVE_ROOM
	MultiManager:emit(param)
end

function MultiManager:emit(param)
    if (param.c == nil) then
		param.c = NetWorkConst.WS.MSG_CODE.MSG
    end 
end

function MultiManager:close()
    WebSocketManager:close()
end

function MultiManager:getStatus()
	WebSocketManager:getStatus()
end

MultiManager.onConnect = nil
MultiManager.onMessage = nil
MultiManager.onCreateRoom = nil
MultiManager.onJoinRoom = nil
MultiManager.onLeaveRoom = nil
MultiManager.onClose = nil
MultiManager.onError = nil
