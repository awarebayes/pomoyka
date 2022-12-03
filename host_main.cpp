#include <iostream>
#include <stdio.h>
#include <stdexcept>
#include <iomanip>
#include <math.h>
#ifdef _WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif


#include "experimental/xrt_device.h"
#include "experimental/xrt_kernel.h"
#include "experimental/xrt_bo.h"
#include "experimental/xrt_ini.h"

#include "gpc_defs.h"
#include "leonhardx64_xrt.h"
#include "gpc_handlers.h"

#define BURST 256
#define EPS 1e+1

union uint64 {
    uint64_t 	u64;
    uint32_t 	u32[2];
    uint16_t 	u16[4];   
    uint8_t 	u8[8];   
};

uint64_t double2ull(double value){

    return * (uint64_t *)&value + (1ull<<63);
}

double ull2double(uint64_t value){

    uint64_t tmp = value + (1ull<<63);
    return *(double *)&tmp;
}

uint64_t rand64(double fMin, double fMax)
{
    double f = (double)rand() / RAND_MAX;
    return double2ull(fMin + f * (fMax - fMin));
}

static void usage()
{
	std::cout << "usage: <xclbin> <sw_kernel>\n\n";
}

int main(int argc, char** argv)
{

	unsigned int cores_count = 0;
	float LNH_CLOCKS_PER_SEC;

	__foreach_core(group, core) cores_count++;

	//Assign xclbin
	if (argc < 3) {
		usage();
		throw std::runtime_error("FAILED_TEST\nNo xclbin specified");
	}

	//Open device #0
	leonhardx64 lnh_inst = leonhardx64(0,argv[1]);
	__foreach_core(group, core)
	{
		lnh_inst.load_sw_kernel(argv[2], group, core);
	}


	// /*
	//  *
	//  * Запись множества из BURST key-value и его последовательное чтение через Global Memory Buffer 
	//  *
	//  */


	//Выделение памяти под буферы gpc2host и host2gpc для каждого ядра и группы
	uint64_t *host2gpc_buffer[LNH_GROUPS_COUNT][LNH_MAX_CORES_IN_GROUP];
	__foreach_core(group, core)
	{
		host2gpc_buffer[group][core] = (uint64_t*) malloc(2*BURST*sizeof(uint64_t));
	}
	uint64_t *gpc2host_buffer[LNH_GROUPS_COUNT][LNH_MAX_CORES_IN_GROUP];
	__foreach_core(group, core)
	{
		gpc2host_buffer[group][core] = (uint64_t*) malloc(2*BURST*sizeof(uint64_t));
	}

	//Создание массива ключей и значений для записи в lnh64
	__foreach_core(group, core)
	{
		for (int i=0;i<BURST;i++) {
			uint64_t key = rand64(-20, 20);
			uint64_t value = double2ull(sin(ull2double(key)));
			//Первый элемент массива uint64_t - key
			host2gpc_buffer[group][core][2*i] = key;
			//Второй uint64_t - value
			host2gpc_buffer[group][core][2*i+1] = value;
//			printf("%d, %.6lf\n", i, ull2double(key));

		}
	}

	//Запуск обработчика insert_burst
	__foreach_core(group, core) {
		lnh_inst.gpc[group][core]->start_async(__event__(insert_burst));
	}

	//DMA запись массива host2gpc_buffer в глобальную память
	__foreach_core(group, core) {
		lnh_inst.gpc[group][core]->buf_write(BURST*2*sizeof(uint64_t),(char*)host2gpc_buffer[group][core]);
	}

	//Ожидание завершения DMA
	__foreach_core(group, core) {
		lnh_inst.gpc[group][core]->buf_write_join();
	}

	uint64_t* user_key = (uint64_t*) malloc(sizeof(uint64_t));
	user_key[0] = double2ull(0.785);
	//Передать количество key-value
	__foreach_core(group, core) {
		lnh_inst.gpc[group][core]->mq_send(BURST);
//		lnh_inst.gpc[group][core]->mq_send(user_key);
	}

	__foreach_core(group, core) {
		lnh_inst.gpc[group][core]->buf_write(sizeof(uint64_t),(char*)user_key);
	}
	//Ожидание завершения DMA
	__foreach_core(group, core) {
		lnh_inst.gpc[group][core]->buf_write_join();
	}

	//Запуск обработчика для последовательного обхода множества ключей
	__foreach_core(group, core) {
		lnh_inst.gpc[group][core]->start_async(__event__(search_burst));
	}

	//Получить количество ключей
	unsigned int count[LNH_GROUPS_COUNT][LNH_MAX_CORES_IN_GROUP];

	__foreach_core(group, core) {
		count[group][core] = lnh_inst.gpc[group][core]->mq_receive();

	}

	//Прочитать количество ключей
	__foreach_core(group, core) {
		lnh_inst.gpc[group][core]->buf_read(sizeof(uint64_t), (char*) gpc2host_buffer[group][core]);
	}

	//Ожидание завершения DMA
	__foreach_core(group, core) {
		lnh_inst.gpc[group][core]->buf_read_join();
	}


	bool error = false;
	double value = 0.0;
	double orig_value = sin(ull2double(*user_key));
	//Проверка целостности данных
	__foreach_core(group, core) {
		double value = ull2double(gpc2host_buffer[group][core][0]);
		if (fabs(value - orig_value) > EPS) {
			error = true;
		}

	}

	__foreach_core(group, core) {
		free(host2gpc_buffer[group][core]);
		free(gpc2host_buffer[group][core]);
	}

	if (!error){
		printf("Тест пройден успешно!\n");
	}
	else {
		printf("Тест завершен с потенциальной ошибкой!\n");
		printf("key: %.6lf\noriginal value: %.6lf\nvalue: %.6lf\n", ull2double(*user_key), ull2double(orig_value), ull2double(value));
	}

	return 0;
}
