// Pruebas unitarias de la lógica de fusión y de la máquina de alarma.
// Se ejecutan en el computador, sin ESP32:
//   c++ -std=c++17 -I stub -I ../monitor_hidrico test_fusion.cpp ../monitor_hidrico/fusion.cpp -o test_fusion && ./test_fusion
#include "fusion.h"
#include <cassert>
int fallos=0;
void check(const char* n,bool ok){printf("%s %s\n",ok?"OK  ":"FALLA",n);if(!ok)fallos++;}
bool cerca(float a,float b){return fabs(a-b)<0.006;}
int main(){
  // Escenarios C-03 (riesgo esperado del protocolo)
  check("C-03a 0.16",cerca(calcularRiesgo(0.80,0.00,0.30),0.16));
  check("C-03b 0.44",cerca(calcularRiesgo(0.20,0.00,0.20),0.44));
  check("C-03c 0.675",cerca(calcularRiesgo(0.15,0.30,0.80),0.675));
  check("C-03d 0.54",cerca(calcularRiesgo(0.60,1.00,0.20),0.54));
  check("C-03e 0.225",cerca(calcularRiesgo(0.95,0.00,1.00),0.225));
  // Ejemplo de evaporación de 02c: T=30, HR=30, LDR=3500 -> 0.785
  float e=calcularIndiceEvaporacion(30,30,3500); printf("   e=%.3f\n",e);
  check("evap ejemplo 0.79",cerca(e,0.785));
  check("evap DHT NaN",cerca(calcularIndiceEvaporacion(NAN,NAN,4095),1.0/3));
  // Causa
  check("causa fuga",estimarCausa(1.0,0.2)==CAUSA_FUGA);
  check("causa evap",estimarCausa(0.2,0.7)==CAUSA_EVAPORACION);
  check("causa ninguna",estimarCausa(0.0,0.9)==CAUSA_NINGUNA);
  // Ventana: 6 muestras bajando 2 cm cada una -> tasa 2 cm/muestra
  VentanaNivel v; for(int i=0;i<6;i++) v.agregar(50-2*i);
  check("ventana llena",v.llena()); check("tasa 2.0",cerca(v.tasaDescenso(),2.0));
  // Máquina de estados
  MaquinaAlarma m;
  m.actualizar(0.70,false); m.actualizar(0.70,false); m.actualizar(0.70,false);
  check("no alerta sin ventana llena",m.estado()!=ALERTA);
  MaquinaAlarma a;
  a.actualizar(0.70,true); check("1 muestra alta -> PRECAUCION",a.estado()==PRECAUCION);
  a.actualizar(0.30,true); check("reinicia persistencia -> NORMAL",a.estado()==NORMAL);
  a.actualizar(0.70,true); a.actualizar(0.70,true); check("2 muestras -> no alerta",a.estado()!=ALERTA);
  a.actualizar(0.70,true); check("3 muestras -> ALERTA",a.estado()==ALERTA);
  a.actualizar(0.62,true); check("0.62 sigue en ALERTA (histeresis)",a.estado()==ALERTA);
  check("silenciar",a.silenciar() && a.estado()==SILENCIADA);
  a.actualizar(0.70,true); check("sigue SILENCIADA",a.estado()==SILENCIADA);
  check("rearmar",a.rearmar() && a.estado()==ALERTA);
  a.silenciar(); a.actualizar(0.55,true); check("rearme automatico -> PRECAUCION",a.estado()==PRECAUCION);
  check("silenciar fuera de ALERTA falla",!a.silenciar());
  a.actualizar(0.66,true);a.actualizar(0.66,true);a.actualizar(0.66,true); check("nueva ALERTA tras rearme",a.estado()==ALERTA);
  a.actualizar(0.30,true); check("ALERTA -> NORMAL directo",a.estado()==NORMAL);
  printf("%d fallas\n",fallos); return fallos;
}
