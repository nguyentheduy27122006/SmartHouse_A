import { signInWithPassword } from "@/lib/firebaseAdmin";
import { CORS, json } from "@/lib/apiAuth";
import { SESSION_COOKIE, makeSession, readSession, sessionCookieOptions } from "@/lib/session";

export const dynamic = "force-dynamic";

export function OPTIONS() {
  return new Response(null, { status: 204, headers: CORS });
}

// GET /api/session → email người đang đăng nhập
export function GET(req) {
  const email = readSession(req);
  return email ? json({ ok: true, email }) : json({ ok: false, login: true, error: "Chưa đăng nhập." }, 401);
}

// POST /api/session   body: { "email": "...", "password": "..." } → đăng nhập
// Dùng tài khoản Firebase Authentication của project (tạo trong Firebase console, tự đăng ký đã tắt).
// Mật khẩu chỉ dùng để kiểm tra với Firebase, không lưu lại.
export async function POST(req) {
  let body = {};
  try { body = await req.json(); } catch { /* body rỗng */ }
  const email = String(body.email || "").trim();
  const password = String(body.password || "");
  if (!email || !password) return json({ ok: false, error: "Nhập email và mật khẩu." }, 400);

  let user;
  try {
    user = await signInWithPassword(email, password);
  } catch (e) {
    if (e.status === 401) return json({ ok: false, error: "Sai email hoặc mật khẩu." }, 401);
    if (e.status === 429) return json({ ok: false, error: "Đăng nhập sai quá nhiều lần. Đợi vài phút rồi thử lại." }, 429);
    return json({ ok: false, error: "Không đăng nhập được: " + e.message }, 502);
  }

  try {
    const res = json({ ok: true, email: user.email || email });
    res.cookies.set(SESSION_COOKIE, makeSession(user.email || email), sessionCookieOptions(req));
    return res;
  } catch (e) {
    return json({ ok: false, error: e.message }, e.status || 500);
  }
}

// DELETE /api/session → đăng xuất
export function DELETE(req) {
  const res = json({ ok: true });
  res.cookies.set(SESSION_COOKIE, "", sessionCookieOptions(req, 0));
  return res;
}
