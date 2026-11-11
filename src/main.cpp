#include <cstdio>
#include <cstdlib>
#include <iostream>
#include "common/Clock.h"
#include "service/AuthService.h"
#include "service/MessageService.h"
#include "service/ProductService.h"
#include "service/ReturnService.h"
#include "service/SearchService.h"
#include "service/TimeoutSweeper.h"
#include "service/TradeService.h"
#include "store/Store.h"
#include "ui/WebUI.h"


int main(int argc, char** argv){
    printf("genshin impact activate!\n");

    Store store;
    SystemClock clock;   // Clock 是接口，实际用系统时钟；测试时可换成 FakeClock

    AuthService auth(store, clock);
    ProductService product(store, clock);
    TradeService trade(store, clock);
    ReturnService returns(store, clock);
    SearchService search(store, clock);
    // 注意声明顺序：messages 必须写在 sweeper 之前（sweeper 析构时先停线程，但仍持有引用）。
    MessageService messages(store, clock);

    // 后台每 30 秒扫描一次超时（REQ-007 / REQ-012 / REQ-014 / REQ-015 / REQ-016 / REQ-006）。
    // 必须声明在上面几个 Service 之后：析构顺序与声明相反，sweeper 会先停掉线程。
    TimeoutSweeper sweeper(product, trade, returns, messages);

    // 端口可选：app.exe 8080
    unsigned short port = 8080;
    if (argc > 1) {
        int parsed = std::atoi(argv[1]);
        if (parsed > 0 && parsed <= 65535) port = static_cast<unsigned short>(parsed);
        else std::cerr << "invalid port: " << argv[1] << ", fallback to 8080\n";
    }

    WebUI ui(auth, product, trade, returns, search, port);
    ui.run();
    return 0;
}
