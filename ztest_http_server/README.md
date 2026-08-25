# DY-EP4 HTTP Image Server

프리즈한 JPEG 이미지를 HTTP body로 받아 장비 MAC별 폴더에 저장하는 Node.js 서버입니다. 외부 npm 패키지가 필요하지 않습니다.

## 실행

Node.js 18 이상에서 실행합니다.

```powershell
cd http_server
npm start
```

기본 주소는 `http://0.0.0.0:8080`이며 이미지는 `http_server/uploads/{장비MAC}`에 저장됩니다.

환경변수로 설정을 변경할 수 있습니다.

```powershell
$env:PORT="8080"
$env:API_KEY="change-this-key"
$env:MAX_IMAGE_BYTES="5242880"
npm start
```

## 상태 확인

```powershell
Invoke-RestMethod http://localhost:8080/health
```

## JPEG 업로드

```powershell
$headers = @{
  "X-Api-Key" = "change-this-key"
  "X-Equipment-Name" = [uri]::EscapeDataString("건조기 1호")
  "X-Captured-At" = "2026-08-02T15:30:00+09:00"
  "X-Image-Width" = "640"
  "X-Image-Height" = "480"
}
Invoke-RestMethod `
  -Method Post `
  -Uri "http://localhost:8080/api/dryers/80F1B2D37D16/captures" `
  -ContentType "image/jpeg" `
  -Headers $headers `
  -InFile ".\sample.jpg"
```

ESP32 요청 규격:

```http
POST /api/dryers/{12자리 Factory Base MAC}/captures
Content-Type: image/jpeg
X-Api-Key: 설정한 API 키
X-Equipment-Name: 건조기 이름
X-Captured-At: ISO 8601 촬영시각
X-Image-Width: 이미지 너비
X-Image-Height: 이미지 높이

[JPEG binary]
```

성공 응답은 HTTP 201입니다.

```json
{
  "success": true,
  "imageId": "20260802T063000Z-...",
  "equipmentCode": "80F1B2D37D16",
  "size": 58231,
  "imageUrl": "/api/dryers/80F1B2D37D16/captures/20260802T063000Z-..."
}
```

응답의 `imageUrl`을 GET 요청하면 저장된 JPEG를 받을 수 있습니다. `API_KEY`를 설정했다면 GET 요청에도 `X-Api-Key` 헤더가 필요합니다.
