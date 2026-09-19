package dev.csgogc.mm.admin;

import jakarta.servlet.http.HttpServletRequest;
import java.io.IOException;
import java.io.InputStream;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import org.springframework.core.io.ClassPathResource;
import org.springframework.http.CacheControl;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.security.authentication.AnonymousAuthenticationToken;
import org.springframework.security.core.Authentication;
import org.springframework.security.web.csrf.CsrfToken;
import org.springframework.stereotype.Controller;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.util.HtmlUtils;

/**
 * Serves the two panel pages. They are templates on the classpath (not static files) because the CSRF token of the
 * current session has to be embedded; everything else the panel needs is in /admin/assets/.
 */
@Controller
public class AdminPageController {

    private final String loginTemplate = read("admin/login.html");
    private final String indexTemplate = read("admin/index.html");

    @GetMapping("/")
    String root() {
        return "redirect:/admin";
    }

    @GetMapping("/admin/login")
    ResponseEntity<String> login(HttpServletRequest request, Authentication authentication,
                                 @RequestParam(required = false) String error,
                                 @RequestParam(required = false) String logout,
                                 @RequestParam(required = false) String locked) {
        if (isLoggedIn(authentication)) {
            return ResponseEntity.status(302).header("Location", "/admin").build();
        }
        // fixed strings only, nothing from the request is echoed
        String message = "";
        if (locked != null) {
            message = "Too many failed attempts. Try again later.";
        } else if (error != null) {
            message = "Invalid username or password.";
        } else if (logout != null) {
            message = "You have been signed out.";
        }
        String html = loginTemplate
                .replace("{{csrf}}", csrf(request))
                .replace("{{message}}", message)
                .replace("{{message_class}}", error != null || locked != null ? "error" : "info");
        return page(html);
    }

    @GetMapping("/admin")
    ResponseEntity<String> index(HttpServletRequest request, Authentication authentication) {
        String html = indexTemplate
                .replace("{{csrf}}", csrf(request))
                .replace("{{user}}", HtmlUtils.htmlEscape(authentication.getName()));
        return page(html);
    }

    private static boolean isLoggedIn(Authentication a) {
        return a != null && a.isAuthenticated() && !(a instanceof AnonymousAuthenticationToken);
    }

    private static String csrf(HttpServletRequest request) {
        CsrfToken token = (CsrfToken) request.getAttribute(CsrfToken.class.getName());
        return token == null ? "" : HtmlUtils.htmlEscape(token.getToken());
    }

    private static ResponseEntity<String> page(String html) {
        return ResponseEntity.ok()
                .contentType(new MediaType("text", "html", StandardCharsets.UTF_8))
                .cacheControl(CacheControl.noStore())
                .body(html);
    }

    private static String read(String path) {
        try (InputStream in = new ClassPathResource(path).getInputStream()) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }
}
