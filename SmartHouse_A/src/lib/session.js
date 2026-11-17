import crypto from "node:crypto";

// Phiên đăng nhập của trang web: cookie httpOnly ký bằng HMAC, giữ 7 ngày. Không lưu gì trên server.
// Khóa ký lấy từ SESSION_SECRET; chưa đặt thì dùng một biến bí mật sẵn có. Đổi khóa thì mọi phiên hết hạn.

export const SESSION_COOKIE = "sh_session";
const MAX_AGE = 7 * 24 * 3600;  // giây

function secret() {
  const s = process.env.SESSION_SECRET || process.env.SMART_HOME_API_KEY || process.env.FIREBASE_PASSWORD || process.env.FIREBASE_SERVICE_ACCOUNT;
  if (!s) throw Object.assign(new Error("Chưa có khóa ký phiên đăng nhập: đặt SESSION_SECRET trong .env hoặc trên Vercel."), { status: 500 });
  return s;
}

const sign = (data) => crypto.createHmac("sha256", secret()).update(data).digest("base64url");

/** Giá trị cookie cho người vừa đăng nhập. */
export function makeSession(email) {
  const payload = Buffer.from(JSON.stringify({ e: email, x: Date.now() + MAX_AGE * 1000 })).toString("base64url");
  return payload + "." + sign(payload);
}

/** Email của người đang đăng nhập (đọc cookie của request), hoặc null nếu chưa đăng nhập hay hết hạn. */
export function readSession(req) {
  const raw = req.cookies?.get(SESSION_COOKIE)?.value;
  if (!raw) return null;
  const [payload, sig] = raw.split(".");
  if (!payload || !sig) return null;
  try {
    const a = Buffer.from(sig), b = Buffer.from(sign(payload));
    if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) return null;
    const { e, x } = JSON.parse(Buffer.from(payload, "base64url").toString());
    return typeof e === "string" && Date.now() < x ? e : null;
  } catch {
    return null;
  }
}

/** Tùy chọn cookie. Chỉ gắn Secure khi trang chạy https (Vercel), để còn thử được ở http://localhost. */
export function sessionCookieOptions(req, maxAge = MAX_AGE) {
  return { httpOnly: true, sameSite: "lax", path: "/", maxAge, secure: req.nextUrl.protocol === "https:" };
}
