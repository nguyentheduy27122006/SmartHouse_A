import { NextResponse } from "next/server";
import { readSession } from "@/lib/session";

// Cho phép trang điều khiển mở từ máy (file://) hoặc từ domain khác gọi API.
export const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, DELETE, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, x-api-key",
  "Cache-Control": "no-store",
};

export const json = (body, status = 200) => NextResponse.json(body, { status, headers: CORS });

/**
 * Chỉ cho người đã đăng nhập trên trang web (cookie phiên, xem /api/session), hoặc script gửi đúng
 * khóa SMART_HOME_API_KEY trong header "x-api-key".
 * Trả về một response lỗi nếu không được phép, hoặc null nếu hợp lệ.
 */
export function checkKey(req) {
  const key = process.env.SMART_HOME_API_KEY;
  if (key && req.headers.get("x-api-key") === key) return null;
  if (readSession(req)) return null;
  return json({ ok: false, login: true, error: "Cần đăng nhập để dùng mạch thật." }, 401);
}
